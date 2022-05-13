#include "redis_hash.h"
#include <utility>
#include <limits>
#include <cmath>
#include <iostream>
#include <rocksdb/status.h>

namespace Redis {
  
rocksdb::Status Hash::GetMetadata(const Slice &ns_key, HashMetadata *metadata) {
  return Database::GetMetadata(kRedisHash, ns_key, metadata);
}

// 获取集合数量
rocksdb::Status Hash::Size(const Slice &user_key, uint32_t *ret) {
  *ret = 0;
  // type ttl size 检测
  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);
  HashMetadata metadata(false);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s;

  *ret = metadata.size;
  return rocksdb::Status::OK();
}

// 返回给定字段的值。如果给定的字段或 key 不存在时，返回 nil 。
rocksdb::Status Hash::Get(const Slice &user_key, const Slice &field, std::string *value) {
  // MultiGet hixme
  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);
  HashMetadata metadata(false);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s;

  LatestSnapShot ss(db_);
  rocksdb::ReadOptions read_options;
  read_options.snapshot = ss.GetSnapShot();
  std::string sub_key;
  InternalKey(ns_key, field, metadata.version, storage_->IsSlotIdEncoded()).Encode(&sub_key);
  return db_->Get(read_options, sub_key, value);
}`

/* Redis Hincrby 命令用于为哈希表中的字段值加上指定增量值。
增量也可以为负数，相当于对指定字段进行减法操作。
如果哈希表的 key 不存在，一个新的哈希表被创建并执行 HINCRBY 命令。
如果指定的字段不存在，那么在执行命令前，字段的值被初始化为 0 。
对一个储存字符串值的字段执行 HINCRBY 命令将造成一个错误。
本操作的值被限制在 64 位(bit)有符号数字表示之内。 */
// 执行 HINCRBY 命令之后，哈希表中字段的值。

rocksdb::Status Hash::IncrBy(const Slice &user_key, const Slice &field, int64_t increment, int64_t *ret) {
  bool exists = false;
  int64_t old_value = 0;

  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);
  HashMetadata metadata;
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok() && !s.IsNotFound()) return s;

  std::string sub_key;
  InternalKey(ns_key, field, metadata.version, storage_->IsSlotIdEncoded()).Encode(&sub_key);
  if (s.ok()) {
    std::string value_bytes;
    std::size_t idx = 0;

    s = db_->Get(rocksdb::ReadOptions(), sub_key, &value_bytes);
    if (!s.ok() && !s.IsNotFound()) return s;
    if (s.ok()) {
      try {
        old_value = std::stoll(value_bytes, &idx);
      } catch (std::exception &e) {
        return rocksdb::Status::InvalidArgument(e.what());
      }
      if (isspace(value_bytes[0]) || idx != value_bytes.size()) {
        return rocksdb::Status::InvalidArgument("value is not an integer");
      }
      exists = true;
    }
  }

  if ((increment < 0 && old_value < 0 && increment < (LLONG_MIN-old_value))
      || (increment > 0 && old_value > 0 && increment > (LLONG_MAX-old_value))) {
    return rocksdb::Status::InvalidArgument("increment or decrement would overflow");
  }

// 更新meta size   slow-log
  *ret = old_value + increment;

  rocksdb::WriteBatch batch;
  WriteBatchLogData log_data(kRedisHash);
  batch.PutLogData(log_data.Encode());
  batch.Put(sub_key, std::to_string(*ret));
  if (!exists) {
    metadata.size += 1;
    std::string bytes;
    metadata.Encode(&bytes);
    batch.Put(metadata_cf_handle_, ns_key, bytes);
  }
  return storage_->Write(rocksdb::WriteOptions(), &batch);
}
/* 
Redis Hincrbyfloat 命令用于为哈希表中的字段值加上指定浮点数增量值。
如果指定的字段不存在，那么在执行命令前，字段的值被初始化为 0 。 */
// 执行 Hincrbyfloat 命令之后，哈希表中字段的值。

rocksdb::Status Hash::IncrByFloat(const Slice &user_key, const Slice &field, double increment, double *ret) {
  bool exists = false;
  float old_value = 0;

  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);
  HashMetadata metadata;
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok() && !s.IsNotFound()) return s;

  std::string sub_key;
  InternalKey(ns_key, field, metadata.version, storage_->IsSlotIdEncoded()).Encode(&sub_key);
  if (s.ok()) {
    std::string value_bytes;
    std::size_t idx = 0;

    s = db_->Get(rocksdb::ReadOptions(), sub_key, &value_bytes);
    if (!s.ok() && !s.IsNotFound()) return s;
    if (s.ok()) {
      try {
        old_value = std::stod(value_bytes, &idx);
      } catch (std::exception &e) {
        return rocksdb::Status::InvalidArgument(e.what());
      }
      if (isspace(value_bytes[0]) || idx != value_bytes.size()) {
        return rocksdb::Status::InvalidArgument("value is not an float");
      }
      exists = true;
    }
  }

  double n = old_value + increment;
  if (std::isinf(n) || std::isnan(n)) {
    return rocksdb::Status::InvalidArgument("increment would produce NaN or Infinity");
  }

  *ret = n;
  rocksdb::WriteBatch batch;
  WriteBatchLogData log_data(kRedisHash);
  batch.PutLogData(log_data.Encode());
  batch.Put(sub_key, std::to_string(*ret));
  if (!exists) {
    metadata.size += 1;
    std::string bytes;
    metadata.Encode(&bytes);
    batch.Put(metadata_cf_handle_, ns_key, bytes);
  }
  return storage_->Write(rocksdb::WriteOptions(), &batch);
}

/* Redis Hmget 命令用于返回哈希表中，一个或多个给定字段的值。
如果指定的字段不存在于哈希表，那么返回一个 nil 值。
 */
// 一个包含多个给定字段关联值的表，表值的排列顺序和指定字段的请求顺序一样。
rocksdb::Status Hash::MGet(const Slice &user_key,
                           const std::vector<Slice> &fields,
                           std::vector<std::string> *values,
                           std::vector<rocksdb::Status> *statuses) {
  values->clear();
  statuses->clear();

  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);
  HashMetadata metadata(false);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) {
    return s;
  }
// multiGet 
  LatestSnapShot ss(db_);
  rocksdb::ReadOptions read_options;
  read_options.snapshot = ss.GetSnapShot();
  std::string sub_key, value;
  for (const auto &field : fields) {
    InternalKey(ns_key, field, metadata.version, storage_->IsSlotIdEncoded()).Encode(&sub_key);
    value.clear();
    auto s = db_->Get(read_options, sub_key, &value);
    if (!s.ok() && !s.IsNotFound()) return s;
    values->emplace_back(value);
    statuses->emplace_back(s);
  }
  return rocksdb::Status::OK();
}

// vector FieldValue
// 如果字段是哈希表中的一个新建字段，并且值设置成功，返回 1 。 如果哈希表中域字段已经存在且旧值已被新值覆盖，返回 0 
rocksdb::Status Hash::Set(const Slice &user_key, const Slice &field, const Slice &value, int *ret) {
  FieldValue fv = {field.ToString(), value.ToString()};
  std::vector<FieldValue> fvs;
  fvs.emplace_back(std::move(fv));
  return MSet(user_key, fvs, false, ret);
}

rocksdb::Status Hash::SetNX(const Slice &user_key, const Slice &field, Slice value, int *ret) {
  FieldValue fv = {field.ToString(), value.ToString()};
  std::vector<FieldValue> fvs;
  fvs.emplace_back(std::move(fv));
  return MSet(user_key, fvs, true, ret);
}

// 被成功删除字段的数量，不包括被忽略的字段。
rocksdb::Status Hash::Delete(const Slice &user_key, const std::vector<Slice> &fields, int *ret) {
  *ret = 0;
  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  HashMetadata metadata(false);
  rocksdb::WriteBatch batch;
  WriteBatchLogData log_data(kRedisHash);
  batch.PutLogData(log_data.Encode());
  LockGuard guard(storage_->GetLockManager(), ns_key);
  //删除之前，先要进行meta检测
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s.IsNotFound() ? rocksdb::Status::OK() : s;

  //进行field 检测
  std::string sub_key, value;
  for (const auto &field : fields) {
    InternalKey(ns_key, field, metadata.version, storage_->IsSlotIdEncoded()).Encode(&sub_key);
    s = db_->Get(rocksdb::ReadOptions(), sub_key, &value);
    if (s.ok()) {
      *ret += 1;
      batch.Delete(sub_key);
    }
  }
  //无filed删除，跳过meta size更新
  if (*ret == 0) {
    return rocksdb::Status::OK();
  }
  //更新meta size
  metadata.size -= *ret;
  std::string bytes;
  metadata.Encode(&bytes);
  batch.Put(metadata_cf_handle_, ns_key, bytes);
  return storage_->Write(rocksdb::WriteOptions(), &batch);
}

// 如果命令执行成功，返回 OK 。 ret 新建字段的数量;meat 更新字段总数；

/* Redis Hsetnx 命令用于为哈希表中不存在的的字段赋值 。
如果哈希表不存在，一个新的哈希表被创建并进行 HSET 操作。
如果字段已经存在于哈希表中，操作无效。
如果 key 不存在，一个新哈希表被创建并执行 HSETNX 命令。 
设置成功，返回 1 。 如果给定字段已经存在且没有操作被执行，返回 0 。
*/

rocksdb::Status Hash::MSet(const Slice &user_key, const std::vector<FieldValue> &field_values, bool nx, int *ret) {
  *ret = 0;
  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);
  HashMetadata metadata;
  // ttl type size 是否符合规范
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok() && !s.IsNotFound()) return s;

  int added = 0;
  bool exists = false;
  rocksdb::WriteBatch batch;
  WriteBatchLogData log_data(kRedisHash);
  batch.PutLogData(log_data.Encode());

  //字段列表
  for (const auto &fv : field_values) {
    exists = false;
    std::string sub_key;
    InternalKey(ns_key, fv.field, metadata.version, storage_->IsSlotIdEncoded()).Encode(&sub_key);
    if (metadata.size > 0) {
      std::string fieldValue;
      s = db_->Get(rocksdb::ReadOptions(), sub_key, &fieldValue); //field是否存在  fixed merge;fieldValue返回是否是新字段
      if (!s.ok() && !s.IsNotFound()) return s;
      if (s.ok()) {
        if (((fieldValue == fv.value) || nx)) continue;
        exists = true;
      }
    }
    if (!exists) added++; //size 处理
    batch.Put(sub_key, fv.value);
  }
  
  if (added > 0) {
    *ret = added;
    metadata.size += added;
    std::string bytes;
    metadata.Encode(&bytes);
    batch.Put(metadata_cf_handle_, ns_key, bytes);
  }
  return storage_->Write(rocksdb::WriteOptions(), &batch);
}

/* Redis Hgetall 命令用于返回哈希表中，所有的字段和值。
在返回值里，紧跟每个字段名(field name)之后是字段的值(value)，所以返回值的长度是哈希表大小的两倍。
 */
// 以列表形式返回哈希表的字段及字段值。 若 key 不存在，返回空列表。
rocksdb::Status Hash::GetAll(const Slice &user_key, std::vector<FieldValue> *field_values, HashFetchType type) {
  field_values->clear();
//meta
  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);
  HashMetadata metadata(false);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s.IsNotFound() ? rocksdb::Status::OK() : s;

  std::string prefix_key, next_version_prefix_key;
  InternalKey(ns_key, "", metadata.version, storage_->IsSlotIdEncoded()).Encode(&prefix_key);
  InternalKey(ns_key, "", metadata.version + 1, storage_->IsSlotIdEncoded()).Encode(&next_version_prefix_key);

  rocksdb::ReadOptions read_options;
  LatestSnapShot ss(db_);
  read_options.snapshot = ss.GetSnapShot();
  //rang query
  rocksdb::Slice upper_bound(next_version_prefix_key);
  read_options.iterate_upper_bound = &upper_bound;
  read_options.fill_cache = false;

  auto iter = db_->NewIterator(read_options);
  for (iter->Seek(prefix_key);
       iter->Valid() && iter->key().starts_with(prefix_key);
       iter->Next()) {
    FieldValue fv;
    if (type == HashFetchType::kOnlyKey) {
      InternalKey ikey(iter->key(), storage_->IsSlotIdEncoded());
      fv.field = ikey.GetSubKey().ToString();
    } else if (type == HashFetchType::kOnlyValue) {
      fv.value = iter->value().ToString();
    } else {
      InternalKey ikey(iter->key(), storage_->IsSlotIdEncoded());
      fv.field = ikey.GetSubKey().ToString();
      fv.value = iter->value().ToString();
    }
    field_values->emplace_back(fv);
  }
  delete iter;
  return rocksdb::Status::OK();
}

/* Redis HSCAN 命令用于迭代哈希表中的键值对。
语法
redis HSCAN 命令基本语法如下：
HSCAN key cursor [MATCH pattern] [COUNT count]
cursor - 游标。
pattern - 匹配的模式。
count - 指定从数据集里返回多少元素，默认值为 10 。
可用版本
>= 2.8.0
返回值
返回的每个元素都是一个元组，每一个元组元素由一个字段(field) 和值（value）组成。 */
rocksdb::Status Hash::Scan(const Slice &user_key,
                           const std::string &cursor,
                           uint64_t limit,
                           const std::string &field_prefix,
                           std::vector<std::string> *fields,
                           std::vector<std::string> *values) {
  return SubKeyScanner::Scan(kRedisHash, user_key, cursor, limit, field_prefix, fields, values);
}

}  // namespace Redis
