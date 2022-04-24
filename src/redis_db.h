
#pragma once

#include <string>
#include <vector>
#include <utility>
#include <map>

#include "redis_metadata.h"
#include "storage.h"

namespace Redis
{
  class Database
  {
  public:
    //  explicit 是避免构造函数的参数自动转换为类对象的标识符
    explicit Database(Engine::Storage *storage, const std::string &ns = "");
    rocksdb::Status GetMetadata(RedisType type, const Slice &ns_key, Metadata *metadata);
    rocksdb::Status GetRawMetadata(const Slice &ns_key, std::string *bytes);
    rocksdb::Status GetRawMetadataByUserKey(const Slice &user_key, std::string *bytes);
    // Redis Expire 命令用于设置 key 的过期时间，key 过期后将不再可用。单位以秒计。
    rocksdb::Status Expire(const Slice &user_key, int timestamp);
    rocksdb::Status Del(const Slice &user_key);
    rocksdb::Status Exists(const std::vector<Slice> &keys, int *ret);
    rocksdb::Status TTL(const Slice &user_key, int *ttl);
    rocksdb::Status Type(const Slice &user_key, RedisType *type);
    // 序列化给定 key ，并返回被序列化的值。
    rocksdb::Status Dump(const Slice &user_key, std::vector<std::string> *infos);
    rocksdb::Status FlushDB();
    rocksdb::Status FlushAll();
    void GetKeyNumStats(const std::string &prefix, KeyNumStats *stats);
    void Keys(std::string prefix, std::vector<std::string> *keys = nullptr, KeyNumStats *stats = nullptr);
    rocksdb::Status Scan(const std::string &cursor,
                         uint64_t limit,
                         const std::string &prefix,
                         std::vector<std::string> *keys,
                         std::string *end_cursor = nullptr);
    rocksdb::Status RandomKey(const std::string &cursor, std::string *key);
    void AppendNamespacePrefix(const Slice &user_key, std::string *output);
    rocksdb::Status FindKeyRangeWithPrefix(const std::string &prefix,
                                           const std::string &prefix_end,
                                           std::string *begin,
                                           std::string *end,
                                           rocksdb::ColumnFamilyHandle *cf_handle = nullptr);
    rocksdb::Status ClearKeysOfSlot(const rocksdb::Slice &ns, int slot);
    rocksdb::Status GetSlotKeysInfo(int slot,
                                    std::map<int, uint64_t> *slotskeys,
                                    std::vector<std::string> *keys,
                                    int count);

  protected:
    Engine::Storage *storage_;
    rocksdb::DB *db_;
    rocksdb::ColumnFamilyHandle *metadata_cf_handle_;
    std::string namespace_;

    // RocksDB通过snapshot控制多时点数据的视图
    // RocksDB内部存储的键实际不是用户视角的键，而是拼接了snapshot sequence number和类型码
    // 内部键 = N字节的用户键（插入键值对时的键）+ 7字节的snapshot sequence number（写入时间戳）+1字节的类型码（标记这个键值对是否是一个删除键值对用的墓碑标记或是否是一个加速“读-修改-写”用的merge标记）
    // Snapshot是以双向链表形式存储哪些时间点的旧版本应该被保留，每个Snapshot实际只是存储了一个8字节的无符号整数（7字节有效），表示需要保留一份该时刻看到的数据的视图。
    // 系统维护当前时间戳，每次有写/删除键值对成功时就把当前时间戳+1。如果是事务写入则对同事务中所有写入的数据使用相同的时间戳。
    // 而从LSM树的视角看，不同时间插入的不同的键值对因为插入时间不同，导致内部键拼接的时间戳不同，可以当作不同的键处理。不同时间插入的相同的用户键之间不会发生冲突，也不会互相覆盖。
    // 每次写入/删除键值对前需要先写WriteAheadLog，崩溃时通过该日志恢复DRAM中memtable和immutable的数据。
    class LatestSnapShot
    {
    public:
      explicit LatestSnapShot(rocksdb::DB *db) : db_(db)
      {
        snapshot_ = db_->GetSnapshot();
      }
      ~LatestSnapShot()
      {
        db_->ReleaseSnapshot(snapshot_);
      }
      const rocksdb::Snapshot *GetSnapShot() { return snapshot_; }

    private:
      rocksdb::DB *db_ = nullptr;
      const rocksdb::Snapshot *snapshot_ = nullptr;
    };
  };

  class SubKeyScanner : public Redis::Database
  {
  public:
    explicit SubKeyScanner(Engine::Storage *storage, const std::string &ns)
        : Database(storage, ns) {}
    rocksdb::Status Scan(RedisType type,
                         const Slice &user_key,
                         const std::string &cursor,
                         uint64_t limit,
                         const std::string &subkey_prefix,
                         std::vector<std::string> *keys,
                         std::vector<std::string> *values = nullptr);
  };

  class WriteBatchLogData
  {
  public:
    WriteBatchLogData() = default;
    // 使用初始化列表来初始化字段 type_=type
    explicit WriteBatchLogData(RedisType type) : type_(type) {}
    // 使用初始化列表来初始化字段 type_=type args_=std::move(args)
    explicit WriteBatchLogData(RedisType type, std::vector<std::string> &&args) : type_(type), args_(std::move(args)) {}

    RedisType GetRedisType();
    std::vector<std::string> *GetArguments();
    std::string Encode();
    Status Decode(const rocksdb::Slice &blob);

  private:
    RedisType type_ = kRedisNone;
    std::vector<std::string> args_;
  };

} // namespace Redis
