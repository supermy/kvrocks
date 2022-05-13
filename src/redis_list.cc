#include "redis_list.h"

#include <stdlib.h>
namespace Redis {

rocksdb::Status List::GetMetadata(const Slice &ns_key, ListMetadata *metadata) {
  return Database::GetMetadata(kRedisList, ns_key, metadata);
}
//meta size
rocksdb::Status List::Size(const Slice &user_key, uint32_t *ret) {
  *ret = 0;

  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);
  ListMetadata metadata(false);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s.IsNotFound() ? rocksdb::Status::OK() : s;
  *ret = metadata.size;
  return rocksdb::Status::OK();
}

rocksdb::Status List::Push(const Slice &user_key, const std::vector<Slice> &elems, bool left, int *ret) {
  return push(user_key, elems, true, left, ret);
}

rocksdb::Status List::PushX(const Slice &user_key, const std::vector<Slice> &elems, bool left, int *ret) {
  return push(user_key, elems, false, left, ret);
}

// Redis Lpush 命令将一个或多个值插入到列表头部。 如果 key 不存在，一个空列表会被创建并执行 LPUSH 操作。 当 key 存在但不是列表类型时，返回一个错误。
rocksdb::Status List::push(const Slice &user_key,
                           std::vector<Slice> elems,
                           bool create_if_missing,
                           bool left,
                           int *ret) {
  *ret = 0;
  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  ListMetadata metadata;
  rocksdb::WriteBatch batch;
  RedisCommand cmd = left ? kRedisCmdLPush : kRedisCmdRPush;
  WriteBatchLogData log_data(kRedisList, {std::to_string(cmd)});
  batch.PutLogData(log_data.Encode());

  LockGuard guard(storage_->GetLockManager(), ns_key);

  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok() && !(create_if_missing && s.IsNotFound())) { //Redis Lpushx 将一个值插入到已存在的列表头部，列表不存在时操作无效。

    return s.IsNotFound() ? rocksdb::Status::OK() : s;
  }
// set无序与hash类似；list 有序需要idx固定排序
  uint64_t index = left ? metadata.head - 1 : metadata.tail; //fixedme 不如rangeQuer+iter.first||iter.last 准确

  for (const auto &elem : elems) {
    std::string index_buf, sub_key;
    PutFixed64(&index_buf, index);
    InternalKey(ns_key, index_buf, metadata.version, storage_->IsSlotIdEncoded()).Encode(&sub_key);
    batch.Put(sub_key, elem);
    left ? --index : ++index; //头尾
  }

  if (left) {//头尾
    metadata.head -= elems.size();
  } else {
    metadata.tail += elems.size();
  }
  std::string bytes;
  metadata.size += elems.size();
  metadata.Encode(&bytes);
  batch.Put(metadata_cf_handle_, ns_key, bytes);
  *ret = metadata.size;
  return storage_->Write(rocksdb::WriteOptions(), &batch);
}

// Redis Lpop 命令用于移除并返回列表的第一个元素。
rocksdb::Status List::Pop(const Slice &user_key, std::string *elem, bool left) {
  elem->clear();

  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);
  ListMetadata metadata(false);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s;

// fixed query & iter.first|iter.last 
  uint64_t index = left ? metadata.head : metadata.tail - 1; 
  std::string buf;
  PutFixed64(&buf, index);
  std::string sub_key;
  InternalKey(ns_key, buf, metadata.version, storage_->IsSlotIdEncoded()).Encode(&sub_key);
  s = db_->Get(rocksdb::ReadOptions(), sub_key, elem);
  if (!s.ok()) {
    // FIXME: should be always exists??
    return s;
  }
  rocksdb::WriteBatch batch;
  RedisCommand cmd = left ? kRedisCmdLPop : kRedisCmdRPop;
  WriteBatchLogData log_data(kRedisList, {std::to_string(cmd)});
  batch.PutLogData(log_data.Encode());
  batch.Delete(sub_key);
  if (metadata.size == 1) {
    batch.Delete(metadata_cf_handle_, ns_key);
  } else {
    std::string bytes;
    metadata.size -= 1;
    left ? ++metadata.head : --metadata.tail;
    metadata.Encode(&bytes);
    batch.Put(metadata_cf_handle_, ns_key, bytes);
  }
  return storage_->Write(rocksdb::WriteOptions(), &batch);
}

/*
 * LRem would remove which value is equal to elem, and count limit the remove number and direction
 * Caution: The LRem timing complexity is O(N), don't use it on a long list
 * The simplified description of LRem Algorithm follows those steps:
 * 1. find out all the index of elems to delete
 * 2. determine to move the remain elems from the left or right by the length of moving elems
 * 3. move the remain elems with overlay
 * 4. trim and delete
 * For example: lrem list hello 0
 * when the list was like this:
 * | E1 | E2 | E3 | hello | E4 | E5 | hello | E6 |
 * the index of elems to delete is [3, 6], left part size is 6 and right part size is 4,
 * so move elems from right to left:
 * => | E1 | E2 | E3 | E4 | E4 | E5 | hello | E6 |
 * => | E1 | E2 | E3 | E4 | E5 | E5 | hello | E6 |
 * => | E1 | E2 | E3 | E4 | E5 | E6 | hello | E6 |
 * then trim the list from tail with num of elems to delete, here is 2.
 * and list would become: | E1 | E2 | E3 | E4 | E5 | E6 |
 */

/* Redis Lrem 根据参数 COUNT 的值，移除列表中与参数 VALUE 相等的元素。
COUNT 的值可以是以下几种：
count > 0 : 从表头开始向表尾搜索，移除与 VALUE 相等的元素，数量为 COUNT 。
count < 0 : 从表尾开始向表头搜索，移除与 VALUE 相等的元素，数量为 COUNT 的绝对值。
count = 0 : 移除表中所有与 VALUE 相等的值。 */
rocksdb::Status List::Rem(const Slice &user_key, int count, const Slice &elem, int *ret) {
  *ret = 0;

  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);
  ListMetadata metadata(false);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s;

  uint64_t index = count >= 0 ? metadata.head : metadata.tail - 1;
  std::string buf, start_key, prefix, next_version_prefix;
  PutFixed64(&buf, index);
  InternalKey(ns_key, buf, metadata.version, storage_->IsSlotIdEncoded()).Encode(&start_key);
  InternalKey(ns_key, "", metadata.version, storage_->IsSlotIdEncoded()).Encode(&prefix);
  InternalKey(ns_key, "", metadata.version + 1, storage_->IsSlotIdEncoded()).Encode(&next_version_prefix);

  bool reversed = count < 0;
  std::vector<uint64_t> to_delete_indexes; //删除清单
  rocksdb::ReadOptions read_options;
  LatestSnapShot ss(db_);
  read_options.snapshot = ss.GetSnapShot();
  rocksdb::Slice upper_bound(next_version_prefix);
  read_options.iterate_upper_bound = &upper_bound; //边界
  rocksdb::Slice lower_bound(prefix);//range query
  read_options.iterate_lower_bound = &lower_bound; //边界
  read_options.fill_cache = false; //

  auto iter = db_->NewIterator(read_options);
  for (iter->Seek(start_key);
       iter->Valid() && iter->key().starts_with(prefix);
       !reversed ? iter->Next() : iter->Prev()) {
    if (iter->value() == elem) {
      InternalKey ikey(iter->key(), storage_->IsSlotIdEncoded());
      Slice sub_key = ikey.GetSubKey();
      GetFixed64(&sub_key, &index);
      to_delete_indexes.emplace_back(index);
      if (static_cast<int>(to_delete_indexes.size()) == abs(count)) break;
    }
  }
  if (to_delete_indexes.empty()) {
    delete iter;
    return rocksdb::Status::NotFound();
  }

  rocksdb::WriteBatch batch;
  WriteBatchLogData log_data(kRedisList, {std::to_string(kRedisCmdLRem), std::to_string(count), elem.ToString()});
  batch.PutLogData(log_data.Encode());

  if (to_delete_indexes.size() == metadata.size) {
    batch.Delete(metadata_cf_handle_, ns_key); //利用version 整体删除
  } else {
    //不用tail&head 更方便准确
    std::string to_update_key, to_delete_key;
    uint64_t min_to_delete_index = !reversed ? to_delete_indexes[0] : to_delete_indexes[to_delete_indexes.size() - 1];
    uint64_t max_to_delete_index = !reversed ? to_delete_indexes[to_delete_indexes.size() - 1] : to_delete_indexes[0];
    uint64_t left_part_len = max_to_delete_index - metadata.head;
    uint64_t right_part_len = metadata.tail - 1 - min_to_delete_index;
    reversed = left_part_len <= right_part_len;
    buf.clear();
    PutFixed64(&buf, reversed ? max_to_delete_index : min_to_delete_index);
    InternalKey(ns_key, buf, metadata.version, storage_->IsSlotIdEncoded()).Encode(&start_key);
    size_t count = 0;
    for (iter->Seek(start_key);
         iter->Valid() && iter->key().starts_with(prefix);
         !reversed ? iter->Next() : iter->Prev()) {
      if (iter->value() != elem || count >= to_delete_indexes.size()) {
        buf.clear();
        PutFixed64(&buf, reversed ? max_to_delete_index-- : min_to_delete_index++);
        InternalKey(ns_key, buf, metadata.version, storage_->IsSlotIdEncoded()).Encode(&to_update_key);
        batch.Put(to_update_key, iter->value());
      } else {
        count++;
      }
    }

    for (uint64_t idx = 0; idx < to_delete_indexes.size(); ++idx) {
      buf.clear();
      PutFixed64(&buf, reversed ? (metadata.head + idx) : (metadata.tail - 1 - idx));
      InternalKey(ns_key, buf, metadata.version, storage_->IsSlotIdEncoded()).Encode(&to_delete_key);
      batch.Delete(to_delete_key);
    }
    if (reversed) {
      metadata.head += to_delete_indexes.size();
    } else {
      metadata.tail -= to_delete_indexes.size();
    }

    //meta size
    metadata.size -= to_delete_indexes.size(); 
    std::string bytes;
    metadata.Encode(&bytes);
    batch.Put(metadata_cf_handle_, ns_key, bytes);
  }

  delete iter;
  *ret = static_cast<int>(to_delete_indexes.size());
  return storage_->Write(rocksdb::WriteOptions(), &batch);
}

// fixed 使用merge在insert point 后移
// Redis Linsert 命令用于在列表的元素前或者后插入元素。当指定元素不存在于列表中时，不执行任何操作。
// 当列表不存在时，被视为空列表，不执行任何操作。
// 类似数组，中间插入数据会导致数据整体后移。如果用链表形式实现则无此问题。主要是看此命令的使用频率。
rocksdb::Status List::Insert(const Slice &user_key, const Slice &pivot, const Slice &elem, bool before, int *ret) {
  *ret = 0;
  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);
  ListMetadata metadata(false);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s;

  std::string buf, start_key, prefix, next_version_prefix;
  uint64_t pivot_index = metadata.head - 1, new_elem_index;
  PutFixed64(&buf, metadata.head);
  InternalKey(ns_key, buf, metadata.version, storage_->IsSlotIdEncoded()).Encode(&start_key);
  InternalKey(ns_key, "", metadata.version, storage_->IsSlotIdEncoded()).Encode(&prefix);
  InternalKey(ns_key, "", metadata.version + 1, storage_->IsSlotIdEncoded()).Encode(&next_version_prefix);

  rocksdb::ReadOptions read_options;
  LatestSnapShot ss(db_);
  read_options.snapshot = ss.GetSnapShot();
  rocksdb::Slice upper_bound(next_version_prefix);
  read_options.iterate_upper_bound = &upper_bound;
  read_options.fill_cache = false;

  auto iter = db_->NewIterator(read_options);
  for (iter->Seek(start_key);
       iter->Valid() && iter->key().starts_with(prefix);
       iter->Next()) {
    if (iter->value() == pivot) {
      InternalKey ikey(iter->key(), storage_->IsSlotIdEncoded());
      Slice sub_key = ikey.GetSubKey();
      GetFixed64(&sub_key, &pivot_index);
      break;
    }
  }
  if (pivot_index == (metadata.head - 1)) {
    delete iter;
    *ret = -1;
    return rocksdb::Status::NotFound();
  }

  rocksdb::WriteBatch batch;
  WriteBatchLogData log_data(kRedisList,
                             {std::to_string(kRedisCmdLInsert),
                              before ? "1" : "0",
                              pivot.ToString(),
                              elem.ToString()});
  batch.PutLogData(log_data.Encode());

  std::string to_update_key;
  uint64_t left_part_len = pivot_index - metadata.head + (before ? 0 : 1);
  uint64_t right_part_len = metadata.tail - 1 - pivot_index + (before ? 1 : 0);
  bool reversed = left_part_len <= right_part_len;
  if ((reversed && !before) || (!reversed && before)) {
    new_elem_index = pivot_index;
  } else {
    new_elem_index = reversed ? --pivot_index : ++pivot_index;
    !reversed ? iter->Next() : iter->Prev();
  }
  for (;
      iter->Valid() && iter->key().starts_with(prefix);
      !reversed ? iter->Next() : iter->Prev()) {
    buf.clear();
    PutFixed64(&buf, reversed ? --pivot_index : ++pivot_index);
    InternalKey(ns_key, buf, metadata.version, storage_->IsSlotIdEncoded()).Encode(&to_update_key);
    batch.Put(to_update_key, iter->value());
  }
  buf.clear();
  PutFixed64(&buf, new_elem_index);
  InternalKey(ns_key, buf, metadata.version, storage_->IsSlotIdEncoded()).Encode(&to_update_key);
  batch.Put(to_update_key, elem);

  if (reversed) {
    metadata.head--;
  } else {
    metadata.tail++;
  }
  metadata.size++;
  std::string bytes;
  metadata.Encode(&bytes);
  batch.Put(metadata_cf_handle_, ns_key, bytes);

  delete iter;
  *ret = metadata.size;
  return storage_->Write(rocksdb::WriteOptions(), &batch);
}
// Redis Lindex 命令用于通过索引获取列表中的元素。你也可以使用负数下标，以 -1 表示列表的最后一个元素， -2 表示列表的倒数第二个元素，以此类推。
rocksdb::Status List::Index(const Slice &user_key, int index, std::string *elem) {
  elem->clear();

  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);
  ListMetadata metadata(false);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s;

  if (index < 0) index += metadata.size;  //fixed me
  if (index < 0 || index >= static_cast<int>(metadata.size)) return rocksdb::Status::NotFound();

  rocksdb::ReadOptions read_options;
  LatestSnapShot ss(db_);
  read_options.snapshot = ss.GetSnapShot();
  std::string buf;
  PutFixed64(&buf, metadata.head + index);//fixed me   rangeQuery+nextX
  std::string sub_key;
  InternalKey(ns_key, buf, metadata.version, storage_->IsSlotIdEncoded()).Encode(&sub_key);
  return db_->Get(read_options, sub_key, elem);
}

// Redis Lrange 返回列表中指定区间内的元素，区间以偏移量 START 和 END 指定。 其中 0 表示列表的第一个元素， 1 表示列表的第二个元素，以此类推。 你也可以使用负数下标，以 -1 表示列表的最后一个元素， -2 表示列表的倒数第二个元素，以此类推。
// The offset can also be negative, -1 is the last element, -2 the penultimate
// Out of range indexes will not produce an error.
// If start is larger than the end of the list, an empty list is returned.
// If stop is larger than the actual end of the list,
// Redis will treat it like the last element of the list.
rocksdb::Status List::Range(const Slice &user_key, int start, int stop, std::vector<std::string> *elems) {
  elems->clear();

  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);
  ListMetadata metadata(false);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s.IsNotFound() ? rocksdb::Status::OK() : s;

  if (start < 0) start = static_cast<int>(metadata.size) + start;
  if (stop < 0) stop = static_cast<int>(metadata.size) + stop;
  if (start > static_cast<int>(metadata.size) || stop < 0 || start > stop) return rocksdb::Status::OK();
  if (start < 0) start = 0;

  std::string buf;
  PutFixed64(&buf, metadata.head + start); //fixed me  如果列表被修剪过，索引就不正确
  std::string start_key, prefix, next_version_prefix;
  InternalKey(ns_key, buf, metadata.version, storage_->IsSlotIdEncoded()).Encode(&start_key);
  InternalKey(ns_key, "", metadata.version, storage_->IsSlotIdEncoded()).Encode(&prefix);
  InternalKey(ns_key, "", metadata.version + 1, storage_->IsSlotIdEncoded()).Encode(&next_version_prefix);

  rocksdb::ReadOptions read_options;
  LatestSnapShot ss(db_);
  read_options.snapshot = ss.GetSnapShot();
  rocksdb::Slice upper_bound(next_version_prefix);
  read_options.iterate_upper_bound = &upper_bound;
  read_options.fill_cache = false;

  auto iter = db_->NewIterator(read_options); //rangeQuery+nextX
  for (iter->Seek(start_key);
       iter->Valid() && iter->key().starts_with(prefix);
       iter->Next()) {
    InternalKey ikey(iter->key(), storage_->IsSlotIdEncoded());
    Slice sub_key = ikey.GetSubKey();
    uint64_t index;
    GetFixed64(&sub_key, &index);
    // index should be always >= start
    if (index > metadata.head + stop) break;
    elems->push_back(iter->value().ToString());
  }
  delete iter;
  return rocksdb::Status::OK();
}

/* 
Redis Lset 通过索引来设置元素的值。
当索引参数超出范围，或对一个空列表进行 LSET 时，返回一个错误。
 */
rocksdb::Status List::Set(const Slice &user_key, int index, Slice elem) {
  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);
  ListMetadata metadata(false);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s;
  if (index < 0) index = metadata.size + index;
  if (index < 0 || index >= static_cast<int>(metadata.size)) {
    return rocksdb::Status::InvalidArgument("index out of range");
  }

  std::string buf, value, sub_key;
  PutFixed64(&buf, metadata.head + index); //fixedme
  InternalKey(ns_key, buf, metadata.version, storage_->IsSlotIdEncoded()).Encode(&sub_key);
  s = db_->Get(rocksdb::ReadOptions(), sub_key, &value);
  if (!s.ok()) {
    return s;
  }
  if (value == elem) return rocksdb::Status::OK();

  rocksdb::WriteBatch batch;
  WriteBatchLogData
      log_data(kRedisList, {std::to_string(kRedisCmdLSet), std::to_string(index)});
  batch.PutLogData(log_data.Encode());
  batch.Put(sub_key, elem); //
  return storage_->Write(rocksdb::WriteOptions(), &batch);
}

rocksdb::Status List::RPopLPush(const Slice &src, const Slice &dst, std::string *elem) {
  RedisType type;
  rocksdb::Status s = Type(dst, &type);
  if (!s.ok()) return s;
  if (type != kRedisNone && type != kRedisList) {
    return rocksdb::Status::InvalidArgument(kErrMsgWrongType);
  }

  s = Pop(src, elem, false);//
  if (!s.ok()) return s;

  int ret;
  std::vector<Slice> elems;
  elems.emplace_back(*elem);
  s = Push(dst, elems, true, &ret);//
  return s;
}
/*
Redis Ltrim 对一个列表进 行修剪(trim)，就是说，让列表只保留指定区间内的元素，不在指定区间之内的元素都将被删除。
下标 0 表示列表的第一个元素，以 1 表示列表的第二个元素，以此类推。 你也可以使用负数下标，以 -1 表示列表的最后一个元素， -2 表示列表的倒数第二个元素，以此类推。 */
//fixed db->DeleteRange(WriteOptions(), start, end); 
// Caution: trim the big list may block the server
rocksdb::Status List::Trim(const Slice &user_key, int start, int stop) {
  uint32_t trim_cnt = 0;
  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);
  ListMetadata metadata(false);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s.IsNotFound() ? rocksdb::Status::OK() : s;

  if (start < 0) start = metadata.size + start;
  if (stop < 0) stop = static_cast<int>(metadata.size) >= -1 * stop ? metadata.size + stop : -1;
  // the result will be empty list when start > stop,
  // or start is larger than the end of list
  if (start > stop) {
    return storage_->Delete(rocksdb::WriteOptions(), metadata_cf_handle_, ns_key);
  }
  if (start < 0) start = 0;

  rocksdb::WriteBatch batch;
  WriteBatchLogData log_data(kRedisList,
                             std::vector<std::string>{std::to_string(kRedisCmdLTrim), std::to_string(start),
                                                      std::to_string(stop)});
  batch.PutLogData(log_data.Encode());
  uint64_t left_index = metadata.head + start;
  uint64_t right_index = metadata.head + stop + 1;
  for (uint64_t i = metadata.head; i < left_index; i++) {
    std::string buf;
    PutFixed64(&buf, i);
    std::string sub_key;
    InternalKey(ns_key, buf, metadata.version, storage_->IsSlotIdEncoded()).Encode(&sub_key);
    batch.Delete(sub_key);
    metadata.head++;
    trim_cnt++;
  }
  auto tail = metadata.tail;
  for (uint64_t i = right_index; i < tail; i++) {
    std::string buf;
    PutFixed64(&buf, i);
    std::string sub_key;
    InternalKey(ns_key, buf, metadata.version, storage_->IsSlotIdEncoded()).Encode(&sub_key);
    batch.Delete(sub_key);
    metadata.tail--;
    trim_cnt++;
  }
  if (metadata.size >= trim_cnt) {
    metadata.size -= trim_cnt;
  } else {
    metadata.size = 0;
  }
  std::string bytes;
  metadata.Encode(&bytes);
  batch.Put(metadata_cf_handle_, ns_key, bytes);
  return storage_->Write(rocksdb::WriteOptions(), &batch);
}
}  // namespace Redis
