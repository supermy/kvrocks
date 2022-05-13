#include <chrono>
#include <utility>
#include <memory>
#include <glog/logging.h>
#include <rocksdb/perf_context.h>
#include <rocksdb/iostats_context.h>

#include "util.h"
#include "redis_reply.h"
#include "redis_request.h"
#include "redis_connection.h"
#include "server.h"
#include "redis_slot.h"

// 协议解析
namespace Redis {
const size_t PROTO_INLINE_MAX_SIZE = 16 * 1024L;
const size_t PROTO_BULK_MAX_SIZE = 512 * 1024L * 1024L;
const size_t PROTO_MULTI_MAX_SIZE = 1024 * 1024L;

// 1.evbuffer以队列的形式管理字节，从尾部添加，从头部取出（FIFO）
// 2.evbuffer内部存储形式是多个独立的连续内存
//加锁解锁
//默认情况下是没有加锁的，多线程并发访问不安全
//第二个参数lock为空，则自动分配一个锁（ 使用evthread_set_lock_creation_callback()设置的锁创建函数）
// Evbuffer是用一个队列实现的，这个队列具有高效的尾插和头删。Evbuffer是用来提高网络 I/O的效率的一个工具，它的源码在 event2/buffer.h中。
Status Request::Tokenize(evbuffer *input) {
  char *line;
  size_t len;
  size_t pipeline_size = 0;
  while (true) {
    switch (state_) {
      case ArrayLen://读取协议元素个数
      // 很多互联网协议使用基于行的格式。evbuffer_readln()函数从evbuffer前面取出一行，用一个新分配的空字符结束的字符串返回这一行。如果n_read_out不是NULL，则它被设置为返回的字符串的字节数。如果没有整行供读取，函数返回空。返回的字符串不包括行结束符。
      //       evbuffer_readln()理解4种行结束格式：
      // l EVBUFFER_EOL_LF
      // 行尾是单个换行符（也就是\n，ASCII值是0x0A）
      // l EVBUFFER_EOL_CRLF_STRICT
      // 行尾是一个回车符，后随一个换行符（也就是\r\n，ASCII值是0x0D 0x0A）
      // l EVBUFFER_EOL_CRLF
      // 行尾是一个可选的回车，后随一个换行符（也就是说，可以是\r\n或者\n）。这种格式对于解析基于文本的互联网协议很有用，因为标准通常要求\r\n的行结束符，而不遵循标准的客户端有时候只使用\n。
      // l EVBUFFER_EOL_ANY
      // 行尾是任意数量、任意次序的回车和换行符。这种格式不是特别有用。它的存在主要是为了向后兼容。
      // （注意，如果使用event_se_mem_functions()覆盖默认的malloc，则evbuffer_readln返回的字符串将由你指定的malloc替代函数分配）
        line = evbuffer_readln(input, &len, EVBUFFER_EOL_CRLF_STRICT);
        if (!line || len <= 0) {
          if (pipeline_size > 128) {
            LOG(INFO) << "Large pipeline detected: " << pipeline_size;
          }
          if (line) {
            free(line);
            continue;
          }
          return Status::OK();
        }
        pipeline_size++;

        svr_->stats_.IncrInbondBytes(len);
        if (line[0] == '*') {
          try {
            //函数功能
            // 将std::string字符串转换为unsigned long long类型
            // 函数参数
            // str : 待转换的字符串
            // idx : 如果idx的指针不为空，则该函数会将idx的值设置为str当前解析完成的数值字符串之后的下一个字符的位置。意义与作用与stdb中的idx类似
            // base : 转换字符所使用的进制数，如果为0，则使用的进制数由字符串的格式决定，默认值为10而不是0
            // 函数返回值
            // 如果成功则返回转换的unsigned long long型数值，如果转换失败，则会抛出invalid_argument异常，如果待转换的字符所代表的数值超出数值类型范围的两倍，则会抛出out_of_range异常
            multi_bulk_len_ = std::stoull(std::string(line + 1, len-1));
          } catch (std::exception &e) {
            free(line);
            return Status(Status::NotOK, "Protocol error: invalid multibulk length");
          }
          if (multi_bulk_len_ > PROTO_MULTI_MAX_SIZE) {
            free(line);
            return Status(Status::NotOK, "Protocol error: invalid multibulk length");
          }
          state_ = BulkLen;
        } else {
          if (len > PROTO_INLINE_MAX_SIZE) {
            free(line);
            return Status(Status::NotOK, "Protocol error: invalid bulk length");
          }
          //set abc 1 直接解析
          Util::Split(std::string(line, len), " \t", &tokens_);
          commands_.emplace_back(std::move(tokens_));
          state_ = ArrayLen;
        }
        free(line);
        break;

      case BulkLen://元素长度
        line = evbuffer_readln(input, &len, EVBUFFER_EOL_CRLF_STRICT);
        if (!line || len <= 0) return Status::OK();
        svr_->stats_.IncrInbondBytes(len);

        if (line[0] != '$') {
          free(line);
          return Status(Status::NotOK, "Protocol error: expected '$'");
        }
        try {

          bulk_len_ = std::stoull(std::string(line + 1, len-1));
        } catch (std::exception &e) {
          free(line);
          return Status(Status::NotOK, "Protocol error: invalid bulk length");
        }
        if (bulk_len_ > PROTO_BULK_MAX_SIZE) {
          free(line);
          return Status(Status::NotOK, "Protocol error: invalid bulk length");
        }
        free(line);
        state_ = BulkData;
        break;

      case BulkData://读取元素数据
        // 函数返回evbuffer存储的字节数
        if (evbuffer_get_length(input) < bulk_len_ + 2) return Status::OK();
        // evbuffer_pullup()函数“线性化”buf前面的size字节，必要时将进行复制或者移动，以保证这些字节是连续的，占据相同的内存块。
        //查看首部N字节的连续空间（首先必须确定这N字节空间是连续的，使用evbuffer_get_contiguous_space ()）
        //size参数为负数，则拷贝首部连续空间的所有数据
        //如果size很大，该接口效率低
        char *data = reinterpret_cast<char *>(evbuffer_pullup(input, bulk_len_ + 2));
        tokens_.emplace_back(data, bulk_len_);
        // evbuffer_drain（）函数的行为与evbuffer_remove（）相同，只是它不进行数据复制：而只是将数据从缓冲区前面移除。成功时返回0，失败时返回-1。
        evbuffer_drain(input, bulk_len_ + 2);
        svr_->stats_.IncrInbondBytes(bulk_len_ + 2);

        --multi_bulk_len_;
        if (multi_bulk_len_ == 0) {
          state_ = ArrayLen;
          // 将[first，last]范围内的元素移到从结果开始的范围内。
          // [first，last]中元素的值将传输到结果所指向的元素。调用之后，[first，last]范围内的元素处于未指定但有效的状态。
          commands_.emplace_back(std::move(tokens_));
          tokens_.clear();
        } else {
          state_ = BulkLen;
        }
        break;
    }
  }
}

}  // namespace Redis
