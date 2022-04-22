#include "encoding.h"

#include <iostream>
#include <string>
#include <glog/logging.h>


#include <limits.h>
#include <float.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
/* Byte ordering detection */
#include <sys/types.h> /* This will likely define BYTE_ORDER */

#include <string>
#include <utility>

#ifndef BYTE_ORDER
#if (BSD >= 199103)
# include <machine/endian.h>
#else
#if defined(linux) || defined(__linux__)
# include <endian.h>
#else
#define LITTLE_ENDIAN   1234    /* least-significant byte first (vax, pc) */
#define BIG_ENDIAN  4321    /* most-significant byte first (IBM, net) */
#define PDP_ENDIAN  3412    /* LSB first in word, MSW first in long (pdp)*/

#if defined(__i386__) || defined(__x86_64__) || defined(__amd64__) || \
  defined(vax) || defined(ns32000) || defined(sun386) || \
  defined(MIPSEL) || defined(_MIPSEL) || defined(BIT_ZERO_ON_RIGHT) || \
  defined(__alpha__) || defined(__alpha)
#define BYTE_ORDER    LITTLE_ENDIAN
#endif

#if defined(__i386__) || defined(__x86_64__) || defined(__amd64__) || \
  defined(vax) || defined(ns32000) || defined(sun386) || \
  defined(MIPSEL) || defined(_MIPSEL) || defined(BIT_ZERO_ON_RIGHT) || \
  defined(__alpha__) || defined(__alpha)
#define BYTE_ORDER    LITTLE_ENDIAN
#endif

#if defined(sel) || defined(pyr) || defined(mc68000) || defined(sparc) || \
  defined(is68k) || defined(tahoe) || defined(ibm032) || defined(ibm370) || \
  defined(MIPSEB) || defined(_MIPSEB) || defined(_IBMR2) || defined(DGUX) ||\
  defined(apollo) || defined(__convex__) || defined(_CRAY) || \
  defined(__hppa) || defined(__hp9000) || \
  defined(__hp9000s300) || defined(__hp9000s700) || \
  defined(BIT_ZERO_ON_LEFT) || defined(m68k) || defined(__sparc)
#define BYTE_ORDER  BIG_ENDIAN
#endif
#endif /* linux */
#endif /* BSD */
#endif /* BYTE_ORDER */

/* Sometimes after including an OS-specific header that defines the
 * endianess we end with __BYTE_ORDER but not with BYTE_ORDER that is what
 * the Redis code uses. In this case let's define everything without the
 * underscores. */
#ifndef BYTE_ORDER
#ifdef __BYTE_ORDER
#if defined(__LITTLE_ENDIAN) && defined(__BIG_ENDIAN)
#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN __LITTLE_ENDIAN
#endif
#ifndef BIG_ENDIAN
#define BIG_ENDIAN __BIG_ENDIAN
#endif
#if (__BYTE_ORDER == __LITTLE_ENDIAN)
#define BYTE_ORDER LITTLE_ENDIAN
#else
#define BYTE_ORDER BIG_ENDIAN
#endif
#endif
#endif
#endif

#if !defined(BYTE_ORDER) || \
    (BYTE_ORDER != BIG_ENDIAN && BYTE_ORDER != LITTLE_ENDIAN)
/* you must determine what the correct bit order is for
     * your compiler - the next line is an intentional error
     * which will force your compiles to bomb until you fix
     * the above macros.
     */
#error "Undefined or invalid BYTE_ORDER"
#endif

void EncodeFixed8(char *buf, uint8_t value) {
  buf[0] = static_cast<uint8_t>(value & 0xff);
}

void EncodeFixed16(char *buf, uint16_t value) {
  if (BYTE_ORDER == BIG_ENDIAN) {
    memcpy(buf, &value, sizeof(value));
  } else {
    buf[0] = static_cast<uint8_t>((value >> 8) & 0xff);
    buf[1] = static_cast<uint8_t>(value & 0xff);
  }
}

void EncodeFixed32(char *buf, uint32_t value) {
  if (BYTE_ORDER == BIG_ENDIAN) {
    memcpy(buf, &value, sizeof(value));
  } else {
    buf[0] = static_cast<uint8_t>((value >> 24) & 0xff);
    buf[1] = static_cast<uint8_t>((value >> 16) & 0xff);
    buf[2] = static_cast<uint8_t>((value >> 8) & 0xff);
    buf[3] = static_cast<uint8_t>(value & 0xff);
  }
}

void EncodeFixed64(char *buf, uint64_t value) {
  if (BYTE_ORDER == BIG_ENDIAN) {
    memcpy(buf, &value, sizeof(value));
  } else {
    buf[0] = static_cast<uint8_t>((value >> 56) & 0xff);
    buf[1] = static_cast<uint8_t>((value >> 48) & 0xff);
    buf[2] = static_cast<uint8_t>((value >> 40) & 0xff);
    buf[3] = static_cast<uint8_t>((value >> 32) & 0xff);
    buf[4] = static_cast<uint8_t>((value >> 24) & 0xff);
    buf[5] = static_cast<uint8_t>((value >> 16) & 0xff);
    buf[6] = static_cast<uint8_t>((value >> 8) & 0xff);
    buf[7] = static_cast<uint8_t>(value & 0xff);
  }
}

// Specifier	Common Equivalent	Signing	Bits	Bytes	Minimum Value	Maximum Value
// uint8_t	unsigned char	Unsigned	8	1	0	255
// size_t主要用于计数，如sizeof函数返回值类型即为size_t。在不同位的机器中所占的位数也不同，size_t是无符号数，ssize_t是有符号数。
// 在32位机器中定义为：typedef unsigned int size_t; （4个字节）
// 在64位机器中定义为：typedef unsigned long size_t;（8个字节）

void PutFixed8(std::string *dst, uint8_t value) {
  LOG(INFO) << "测试";
  LOG(INFO) << (value & 0xff);
  char buf[1];
  buf[0] = static_cast<uint8_t>(value & 0xff);
  dst->append(buf, 1);
}

void PutFixed16(std::string *dst, uint16_t value) {
  char buf[sizeof(value)];
  EncodeFixed16(buf, value);
  dst->append(buf, sizeof(buf));
}

void PutFixed32(std::string *dst, uint32_t value) {
  char buf[sizeof(value)];
  EncodeFixed32(buf, value);
  dst->append(buf, sizeof(buf));
}

void PutFixed64(std::string *dst, uint64_t value) {
  char buf[sizeof(value)];
  EncodeFixed64(buf, value);
  dst->append(buf, sizeof(buf));
}

void PutDouble(std::string *dst, double value) {
  uint64_t u64;
  memcpy(&u64, &value, sizeof(value));
  auto ptr = &u64;
  if ((*ptr >> 63) == 1) {
    // signed bit would be zero
    *ptr ^= 0xffffffffffffffff;
  } else {
    // signed bit would be one
    *ptr |= 0x8000000000000000;
  }
  PutFixed64(dst, *ptr);
}

bool GetFixed8(rocksdb::Slice *input, uint8_t *value) {
  const char *data;
  if (input->size() < sizeof(uint8_t)) {
    return false;
  }
  data = input->data();
  *value = static_cast<uint8_t>(data[0] & 0xff);
  input->remove_prefix(sizeof(uint8_t));
  return true;
}

bool GetFixed64(rocksdb::Slice *input, uint64_t *value) {
  if (input->size() < sizeof(uint64_t)) {
    return false;
  }
  *value = DecodeFixed64(input->data());
  input->remove_prefix(sizeof(uint64_t));
  return true;
}

bool GetFixed32(rocksdb::Slice *input, uint32_t *value) {
  if (input->size() < sizeof(uint32_t)) {
    return false;
  }
  *value = DecodeFixed32(input->data());
  input->remove_prefix(sizeof(uint32_t));
  return true;
}

bool GetFixed16(rocksdb::Slice *input, uint16_t *value) {
  if (input->size() < sizeof(uint16_t)) {
    return false;
  }
  *value = DecodeFixed16(input->data());
  input->remove_prefix(sizeof(uint16_t));
  return true;
}

bool GetDouble(rocksdb::Slice *input, double *value) {
  if (input->size() < sizeof(double)) return false;
  *value = DecodeDouble(input->data());
  input->remove_prefix(sizeof(double));
  return true;
}

uint16_t DecodeFixed16(const char *ptr) {
  if (BYTE_ORDER == BIG_ENDIAN) {
    uint16_t value;
    memcpy(&value, ptr, sizeof(value));
    return value;
  } else {
    return ((static_cast<uint16_t>(static_cast<uint8_t>(ptr[1])))
        | (static_cast<uint16_t>(static_cast<uint8_t>(ptr[0])) << 8));
  }
}

uint32_t DecodeFixed32(const char *ptr) {
  if (BYTE_ORDER == BIG_ENDIAN) {
    uint32_t value;
    memcpy(&value, ptr, sizeof(value));
    return value;
  } else {
    return ((static_cast<uint32_t>(static_cast<uint8_t>(ptr[3])))
        | (static_cast<uint32_t>(static_cast<uint8_t>(ptr[2])) << 8)
        | (static_cast<uint32_t>(static_cast<uint8_t>(ptr[1])) << 16)
        | (static_cast<uint32_t>(static_cast<uint8_t>(ptr[0])) << 24));
  }
}

uint64_t DecodeFixed64(const char *ptr) {
  if (BYTE_ORDER == BIG_ENDIAN) {
    uint64_t value;
    memcpy(&value, ptr, sizeof(value));
    return value;
  } else {
    uint64_t hi = DecodeFixed32(ptr);
    uint64_t lo = DecodeFixed32(ptr+4);
    return (hi << 32) | lo;
  }
}

double DecodeDouble(const char *ptr) {
  uint64_t decoded = DecodeFixed64(ptr);
  if ((decoded>>63) == 0) {
    decoded ^= 0xffffffffffffffff;
  } else {
    decoded &= 0x7fffffffffffffff;
  }
  double value;
  memcpy(&value, &decoded, sizeof(value));
  return value;
}



int main(int argc, char** argv) {
    FLAGS_alsologtostderr = 1;
    google::InitGoogleLogging(argv[0]);

    //通过SetLogDestination可能没有设置log_dir标志位的方式方便(会遗漏一些日志)
    //google::SetLogDestination(google::GLOG_INFO, "/tmp/today");

    //标志位
    FLAGS_colorlogtostderr=true;  //设置输出颜色
    FLAGS_v = std::atoi(argv[1]); //设置最大显示等级(超过该等级将不记录log)
    FLAGS_log_dir = "logs";

    LOG(INFO) << "Found " << google::COUNTER << " arguments!";

    // assert 检测文件是否存在
    CHECK(access(argv[2], 0) != -1) << "No such file: "<<argv[2];

    LOG(INFO) << "I am INFO!";
    LOG(WARNING) << "I am WARNING!";
    LOG(ERROR) << "I am ERROR!";

    // LOG_IF(INFO, num_cookies > 10) << "Got lots of cookies"; //当条件满足时输出日志
    LOG_EVERY_N(INFO, 10) << "Got the " << google::COUNTER << "th cookie"; //第一次执行以后每隔十次记录一次log
    // LOG_IF_EVERY_N(INFO, (size > 1024), 10) //上面两者的结合
    LOG_FIRST_N(INFO, 20); // 此语句执行的前20次都输出日志；后面执行不输出日志


    //VLOG用来自定义日志, 可以在括号内指定log级别
    VLOG(1) << "[Custom log(VLOG)] Level 1!";
    VLOG(2) << "[Custom log(VLOG)] Level 2!";
    VLOG(3) << "[Custom log(VLOG)] Level 3!";
    VLOG(4) << "[Custom log(VLOG)] Level 4! This is used for detailed message which need not to be printed each time.";
    VLOG(5) << "[Custom log(VLOG)] Level 5! On this level messages are print as detail as possible for debugging.";
    // LOG(FATAL) << "I am FATAL!";
    char buf[1];
    buf[0] = static_cast<uint8_t>(8 & 0xff);


    uint8_t value=static_cast<uint8_t>(buf[0] & 0xff);

    LOG(INFO) << value;

    return 0;
}