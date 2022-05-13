#include "redis_set.h"

#include <map>
#include <iostream>
#include <memory>

//crud+randget+差集并集交集+覆盖存储(del-range)
namespace Redis {

rocksdb::Status Set::GetMetadata(const Slice &ns_key, SetMetadata *metadata) {
  return Database::GetMetadata(kRedisSet, ns_key, metadata);
}

//version 不用删除原有的数据
// Make sure members are uniq before use Overwrite
rocksdb::Status Set::Overwrite(Slice user_key, const std::vector<std::string> &members) {
  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);
  SetMetadata metadata;
  rocksdb::WriteBatch batch;
  WriteBatchLogData log_data(kRedisSet);
  batch.PutLogData(log_data.Encode());
  std::string sub_key;
  for (const auto &member : members) {
    InternalKey(ns_key, member, metadata.version, storage_->IsSlotIdEncoded()).Encode(&sub_key);
    batch.Put(sub_key, Slice());
  }
  //overwrite size不变
  metadata.size = static_cast<uint32_t>(members.size());
  std::string bytes;
  metadata.Encode(&bytes);
  batch.Put(metadata_cf_handle_, ns_key, bytes);
  return storage_->Write(rocksdb::WriteOptions(), &batch);
}

/* 
Redis Sadd 命令将一个或多个成员元素加入到集合中，已经存在于集合的成员元素将被忽略。
假如集合 key 不存在，则创建一个只包含添加的元素作成员的集合。
当集合 key 不是集合类型时，返回一个错误。 */
// 
rocksdb::Status Set::Add(const Slice &user_key, const std::vector<Slice> &members, int *ret) {
  *ret = 0;
// 1.meta type ttl size
  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);
  SetMetadata metadata;
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok() && !s.IsNotFound()) return s;
// 2. log member
  std::string value;
  rocksdb::WriteBatch batch;
  WriteBatchLogData log_data(kRedisSet);
  batch.PutLogData(log_data.Encode());
  std::string sub_key;
  for (const auto &member : members) {
    InternalKey(ns_key, member, metadata.version, storage_->IsSlotIdEncoded()).Encode(&sub_key);
    s = db_->Get(rocksdb::ReadOptions(), sub_key, &value);
    if (s.ok()) continue;  //跳过已有元素
    batch.Put(sub_key, Slice());
    *ret += 1;
  }
// 3. meta update size
  if (*ret > 0) {
    metadata.size += *ret;
    std::string bytes;
    metadata.Encode(&bytes);
    batch.Put(metadata_cf_handle_, ns_key, bytes);
  }
  return storage_->Write(rocksdb::WriteOptions(), &batch);
}

/* 
Redis Spop 命令用于移除集合中的指定 key 的一个或多个随机元素，移除后会返回移除的元素。
该命令类似 Srandmember 命令，但 SPOP 将随机元素从集合中移除并返回，而 Srandmember 则仅仅返回随机元素，而不对集合进行任何改动。 */
rocksdb::Status Set::Remove(const Slice &user_key, const std::vector<Slice> &members, int *ret) {
  *ret = 0;

  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);
  SetMetadata metadata(false);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s.IsNotFound() ? rocksdb::Status::OK() : s;

  std::string value, sub_key;
  rocksdb::WriteBatch batch;
  WriteBatchLogData log_data(kRedisSet);
  batch.PutLogData(log_data.Encode());

  for (const auto &member : members) {
    InternalKey(ns_key, member, metadata.version, storage_->IsSlotIdEncoded()).Encode(&sub_key);
    s = db_->Get(rocksdb::ReadOptions(), sub_key, &value);
    if (!s.ok()) continue;
    batch.Delete(sub_key);
    *ret += 1;
  }

  if (*ret > 0) {
    if (static_cast<int>(metadata.size) != *ret) {
      metadata.size -= *ret;
      std::string bytes;
      metadata.Encode(&bytes);
      batch.Put(metadata_cf_handle_, ns_key, bytes);
    } else {
      batch.Delete(metadata_cf_handle_, ns_key);
    }
  }
  return storage_->Write(rocksdb::WriteOptions(), &batch);
}

// Redis Scard 命令返回集合中元素的数量。
rocksdb::Status Set::Card(const Slice &user_key, int *ret) {
  *ret = 0;
  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  SetMetadata metadata(false);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s.IsNotFound() ? rocksdb::Status::OK() : s;
  *ret = metadata.size;
  return rocksdb::Status::OK();
}

// Redis Smembers 命令返回集合中的所有的成员。 不存在的集合 key 被视为空集合。
rocksdb::Status Set::Members(const Slice &user_key, std::vector<std::string> *members) {
  members->clear();

  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  SetMetadata metadata(false);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s.IsNotFound() ? rocksdb::Status::OK() : s;

  std::string prefix, next_version_prefix;
  InternalKey(ns_key, "", metadata.version, storage_->IsSlotIdEncoded()).Encode(&prefix);
  InternalKey(ns_key, "", metadata.version + 1, storage_->IsSlotIdEncoded()).Encode(&next_version_prefix);

  rocksdb::ReadOptions read_options;
  LatestSnapShot ss(db_);
  read_options.snapshot = ss.GetSnapShot();
  rocksdb::Slice upper_bound(next_version_prefix);
  read_options.iterate_upper_bound = &upper_bound;
  read_options.fill_cache = false;

  auto iter = db_->NewIterator(read_options);
  for (iter->Seek(prefix);
       iter->Valid() && iter->key().starts_with(prefix);
       iter->Next()) {
    InternalKey ikey(iter->key(), storage_->IsSlotIdEncoded());
    members->emplace_back(ikey.GetSubKey().ToString());
  }
  delete iter;
  return rocksdb::Status::OK();
}

// 如果成员元素是集合的成员，返回 1 。 如果成员元素不是集合的成员，或 key 不存在，返回 0 。
rocksdb::Status Set::IsMember(const Slice &user_key, const Slice &member, int *ret) {
  *ret = 0;

  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  SetMetadata metadata(false);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s.IsNotFound() ? rocksdb::Status::OK() : s;

  rocksdb::ReadOptions read_options;
  LatestSnapShot ss(db_);
  read_options.snapshot = ss.GetSnapShot();
  std::string sub_key;
  InternalKey(ns_key, member, metadata.version, storage_->IsSlotIdEncoded()).Encode(&sub_key);
  std::string value;
  s = db_->Get(read_options, sub_key, &value); //
  if (s.ok()) {
    *ret = 1;
  }
  return rocksdb::Status::OK();
}

// 随机=顺序数量
rocksdb::Status Set::Take(const Slice &user_key, std::vector<std::string> *members, int count, bool pop) {
  int n = 0;
  members->clear();
  if (count <= 0) return rocksdb::Status::OK();

  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  std::unique_ptr<LockGuard> lock_guard;
  if (pop) lock_guard = std::unique_ptr<LockGuard>(new LockGuard(storage_->GetLockManager(), ns_key));

  SetMetadata metadata(false);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s.IsNotFound() ? rocksdb::Status::OK() : s;

  rocksdb::WriteBatch batch;
  WriteBatchLogData log_data(kRedisSet);
  batch.PutLogData(log_data.Encode());

  std::string prefix, next_version_prefix;
  InternalKey(ns_key, "", metadata.version, storage_->IsSlotIdEncoded()).Encode(&prefix);
  InternalKey(ns_key, "", metadata.version + 1, storage_->IsSlotIdEncoded()).Encode(&next_version_prefix);

  rocksdb::ReadOptions read_options;
  LatestSnapShot ss(db_);
  read_options.snapshot = ss.GetSnapShot();
  rocksdb::Slice upper_bound(next_version_prefix);//rand query
  read_options.iterate_upper_bound = &upper_bound;
  read_options.fill_cache = false;

  auto iter = db_->NewIterator(read_options);
  for (iter->Seek(prefix);
       iter->Valid() && iter->key().starts_with(prefix);
       iter->Next()) { //size+next 随机获取
    InternalKey ikey(iter->key(), storage_->IsSlotIdEncoded());
    members->emplace_back(ikey.GetSubKey().ToString());
    if (pop) batch.Delete(iter->key()); //删除 pop
    if (++n >= count) break;//指定数量
  }
  delete iter;
  if (pop && n > 0) {
    metadata.size -= n;
    std::string bytes;
    metadata.Encode(&bytes);
    batch.Put(metadata_cf_handle_, ns_key, bytes);
  }
  return storage_->Write(rocksdb::WriteOptions(), &batch);
}

// Redis Srem 命令用于移除集合中的一个或多个成员元素，不存在的成员元素会被忽略。
// 当 key 不是集合类型，返回一个错误。
rocksdb::Status Set::Move(const Slice &src, const Slice &dst, const Slice &member, int *ret) {
  RedisType type;
  rocksdb::Status s = Type(dst, &type);
  if (!s.ok()) return s;
  if (type != kRedisNone && type != kRedisSet) {
    return rocksdb::Status::InvalidArgument(kErrMsgWrongType);
  }

  std::vector<Slice> members{member};
  s = Remove(src, members, ret);
  if (!s.ok() || *ret == 0) {
    return s;
  }
  return Add(dst, members, ret);
}

rocksdb::Status Set::Scan(const Slice &user_key,
                          const std::string &cursor,
                          uint64_t limit,
                          const std::string &member_prefix,
                          std::vector<std::string> *members) {
  return SubKeyScanner::Scan(kRedisSet, user_key, cursor, limit, member_prefix, members);
}

/* 
Redis Sdiff 命令返回第一个集合与其他集合之间的差异，也可以认为说第一个集合中独有的元素。不存在的集合 key 将视为空集。
差集的结果来自前面的 FIRST_KEY ,而不是后面的 OTHER_KEY1，也不是整个 FIRST_KEY OTHER_KEY1..OTHER_KEYN 的差集。 */
/*
 * Returns the members of the set resulting from the difference between
 * the first set and all the successive sets. For example:
 * key1 = {a,b,c,d}
 * key2 = {c}
 * key3 = {a,c,e}
 * DIFF key1 key2 key3 = {b,d}
 */
rocksdb::Status Set::Diff(const std::vector<Slice> &keys, std::vector<std::string> *members) {
  members->clear();
  std::vector<std::string> source_members;
  auto s = Members(keys[0], &source_members);
  if (!s.ok()) return s;

/* 
Item	Description
begin()	返回指向map头部的迭代器
clear()	删除所有元素
empty()	判断map是否为空
end()	返回指向map末尾的迭代器
erase()	删除一个元素
find()	查找一个元素
insert()	插入元素
size()	返回map中元素的个数
 */
 /*  std::map<std::string, bool> exclude_members;
  1.push_back 在数组的最后添加一个数据
2.pop_back 去掉数组的最后一个数据
3.at 得到编号位置的数据
4.begin 得到数组头的指针
5.end 得到数组的最后一个单元+1的指针
6．front 得到数组头的引用
7.back 得到数组的最后一个单元的引用
8.max_size 得到vector最大可以是多大
9.capacity 当前vector分配的大小
10.size 当前使用数据的大小
11.resize 改变当前使用数据的大小，如果它比当前使用的大，者填充默认值
12.reserve 改变当前vecotr所分配空间的大小
13.erase 删除指针指向的数据项
14.clear 清空当前的vector
15.rbegin 将vector反转后的开始指针返回(其实就是原来的end-1)
16.rend 将vector反转构的结束指针返回(其实就是原来的begin-1)
17.empty 判断vector是否为空
18.swap 与另一个vector交换数据 */
  std::vector<std::string> target_members;
  //其他集合所有keys=true
  for (size_t i = 1; i < keys.size(); i++) {
    s = Members(keys[i], &target_members);
    if (!s.ok()) return s;
    for (const auto &member : target_members) {
      exclude_members[member] = true;
    }
  }
  // 求差异 mapTest.erase(it++);
  for (const auto &member : source_members) {
    if (exclude_members.find(member) == exclude_members.end()) {
      members->push_back(member); 
    }
  }
  return rocksdb::Status::OK();
}

/* 并集
 * Returns the members of the set resulting from the union of all the given sets.
 * For example:
 * key1 = {a,b,c,d}
 * key2 = {c}
 * key3 = {a,c,e}
 * UNION key1 key2 key3 = {a,b,c,d,e}
 */
rocksdb::Status Set::Union(const std::vector<Slice> &keys, std::vector<std::string> *members) {
  members->clear();

  std::map<std::string, bool> union_members;
  std::vector<std::string> target_members;
  for (size_t i = 0; i < keys.size(); i++) {
    auto s = Members(keys[i], &target_members);
    if (!s.ok()) return s;
    for (const auto &member : target_members) {
      union_members[member] = true;
    }
  }
  // emplace_back() 和 push_back() 的区别，就在于底层实现的机制不同。push_back() 向容器尾部添加元素时，首先会创建这个元素，然后再将这个元素拷贝或者移动到容器中（如果是拷贝的话，事后会自行销毁先前创建的这个元素）；而 emplace_back() 在实现时，则是直接在容器尾部创建这个元素，省去了拷贝或移动元素的过程。
  for (const auto &iter : union_members) {
    members->emplace_back(iter.first); //
  }
  return rocksdb::Status::OK();
}

/*交集
 * Returns the members of the set resulting from the intersection of all the given sets.
 * For example:
 * key1 = {a,b,c,d}
 * key2 = {c}
 * key3 = {a,c,e}
 * INTER key1 key2 key3 = {c}
 */
rocksdb::Status Set::Inter(const std::vector<Slice> &keys, std::vector<std::string> *members) {
  members->clear();

  std::map<std::string, size_t> member_counters;//key计数
  std::vector<std::string> target_members;
  auto s = Members(keys[0], &target_members);
  if (!s.ok() || target_members.empty()) return s;
  for (const auto &member : target_members) {
    member_counters[member] = 1;
  }
  for (size_t i = 1; i < keys.size(); i++) {
    auto s = Members(keys[i], &target_members);
    if (!s.ok() || target_members.empty()) return s;
    for (const auto &member : target_members) {
      if (member_counters.find(member) == member_counters.end()) continue;
      member_counters[member]++;
    }
  }

  for (const auto &iter : member_counters) {
    if (iter.second == keys.size()) {  // all the sets contain this member
      members->emplace_back(iter.first);
    }
  }
  return rocksdb::Status::OK();
}

rocksdb::Status Set::DiffStore(const Slice &dst, const std::vector<Slice> &keys, int *ret) {
  *ret = 0;
  std::vector<std::string> members;
  auto s = Diff(keys, &members);
  if (!s.ok()) return s;
  *ret = static_cast<int>(members.size());
  return Overwrite(dst, members);
}

rocksdb::Status Set::UnionStore(const Slice &dst, const std::vector<Slice> &keys, int *ret) {
  *ret = 0;
  std::vector<std::string> members;
  auto s = Union(keys, &members);
  if (!s.ok()) return s;
  *ret = static_cast<int>(members.size());
  return Overwrite(dst, members);
}

rocksdb::Status Set::InterStore(const Slice &dst, const std::vector<Slice> &keys, int *ret) {
  *ret = 0;
  std::vector<std::string> members;
  auto s = Inter(keys, &members);
  if (!s.ok()) return s;
  *ret = static_cast<int>(members.size());
  return Overwrite(dst, members);
}
}  // namespace Redis
