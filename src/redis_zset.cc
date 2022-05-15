#include "redis_zset.h"

#include <math.h>
#include <map>
#include <limits>
#include <cmath>
#include <memory>
#include <set>

namespace Redis {

rocksdb::Status ZSet::GetMetadata(const Slice &ns_key, ZSetMetadata *metadata) {
  return Database::GetMetadata(kRedisZSet, ns_key, metadata);
}

/* ZSET_INCR
Redis Zadd 命令用于将一个或多个成员元素及其分数值加入到有序集当中。
如果某个成员已经是有序集的成员，那么更新这个成员的分数值，并通过重新插入这个成员元素，来保证该成员在正确的位置上。
分数值可以是整数值或双精度浮点数。
如果有序集合 key 不存在，则创建一个空的有序集并执行 ZADD 操作。
当 key 存在但不是有序集类型时，返回一个错误。 */
rocksdb::Status ZSet::Add(const Slice &user_key, uint8_t flags, std::vector<MemberScore> *mscores, int *ret) {
  *ret = 0;

  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);
// meta type ttl size
  LockGuard guard(storage_->GetLockManager(), ns_key);
  ZSetMetadata metadata;
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok() && !s.IsNotFound()) return s;

  int added = 0;
  rocksdb::WriteBatch batch;
  WriteBatchLogData log_data(kRedisZSet);
  batch.PutLogData(log_data.Encode());

  std::string member_key;
  std::set<std::string> added_member_keys; //member_keys
  for (int i = static_cast<int>(mscores->size()-1); i >= 0; i--) {
    InternalKey(ns_key, (*mscores)[i].member, metadata.version,
                storage_->IsSlotIdEncoded()).Encode(&member_key);

    // Fix the corner case that adds the same member which may add the score
    // column family many times and cause problems in the ZRANGE command.
    //
    // For example, we add members with `ZADD mykey 1 a 2 a` and `ZRANGE mykey 0 1`
    // return only one member(`a`) was expected but got the member `a` twice now.
    //
    // The root cause of this issue was the score key  was composed by member and score,
    // so the last one can't overwrite the previous when the score was different.
    // A simple workaround was add those members with reversed order and skip the member if has added.
    if (added_member_keys.find(member_key) != added_member_keys.end()) { //去重
      continue;
    }
    added_member_keys.insert(member_key);

    if (metadata.size > 0) {
      std::string old_score_bytes;
      s = db_->Get(rocksdb::ReadOptions(), member_key, &old_score_bytes); //
      if (!s.ok() && !s.IsNotFound()) return s;
      if (s.ok()) {  //member_key存在的处理逻辑  merge 更新分数取回原有分数 batch是顺序串行执行？
        double old_score = DecodeDouble(old_score_bytes.data());
        if (flags == ZSET_INCR) { //ZSET_INCR
          (*mscores)[i].score += old_score;  
          // 判断是否为非数字的std::isnan
          if (std::isnan((*mscores)[i].score)) {
            return rocksdb::Status::InvalidArgument("resulting score is not a number (NaN)");
          }
        }
        // 数据相同跳过
        if ((*mscores)[i].score != old_score) {
          // merge and batch 是否有序 put null==delete ?
          old_score_bytes.append((*mscores)[i].member);
          std::string old_score_key;
          InternalKey(ns_key, old_score_bytes, metadata.version, storage_->IsSlotIdEncoded()).Encode(&old_score_key);
          batch.Delete(score_cf_handle_, old_score_key); //old_score_key 删除分数主键

          std::string new_score_bytes, new_score_key;
          PutDouble(&new_score_bytes, (*mscores)[i].score);
          batch.Put(member_key, new_score_bytes); //member_key

          new_score_bytes.append((*mscores)[i].member);
          InternalKey(ns_key, new_score_bytes, metadata.version, storage_->IsSlotIdEncoded()).Encode(&new_score_key);
          batch.Put(score_cf_handle_, new_score_key, Slice()); //new_score_key
        }
        continue;
      }
    }

//member_key新增的处理逻辑
    std::string score_bytes, score_key;
    PutDouble(&score_bytes, (*mscores)[i].score);
    batch.Put(member_key, score_bytes); //member_key

// score_key
    score_bytes.append((*mscores)[i].member);
    InternalKey(ns_key, score_bytes, metadata.version, storage_->IsSlotIdEncoded()).Encode(&score_key);
    batch.Put(score_cf_handle_, score_key, Slice());
    added++;
  }

  //meta size
  if (added > 0) {
    *ret = added;
    metadata.size += added;
    std::string bytes;
    metadata.Encode(&bytes);
    batch.Put(metadata_cf_handle_, ns_key, bytes);
  }
  return storage_->Write(rocksdb::WriteOptions(), &batch);
}

//meta size
rocksdb::Status ZSet::Card(const Slice &user_key, int *ret) {
  *ret = 0;

  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  ZSetMetadata metadata(false);//不构造版本
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s.IsNotFound()? rocksdb::Status::OK():s;
  *ret = metadata.size;  //meta size
  return rocksdb::Status::OK();
}

// Redis Zcount 命令用于计算有序集合中指定分数区间的成员数量。
rocksdb::Status ZSet::Count(const Slice &user_key, const ZRangeSpec &spec, int *ret) {
  *ret = 0;
  return RangeByScore(user_key, spec, nullptr, ret);
}

rocksdb::Status ZSet::IncrBy(const Slice &user_key, const Slice &member, double increment, double *score) {
  int ret;
  std::vector<MemberScore> mscores;
  mscores.emplace_back(MemberScore{member.ToString(), increment});
  rocksdb::Status s = Add(user_key, ZSET_INCR, &mscores, &ret);
  if (!s.ok()) return s;
  *score = mscores[0].score;
  return rocksdb::Status::OK();
}

rocksdb::Status ZSet::Pop(const Slice &user_key, int count, bool min, std::vector<MemberScore> *mscores) {
  mscores->clear();

  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);
  ZSetMetadata metadata(false);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s.IsNotFound()? rocksdb::Status::OK():s;
  if (count <=0) return rocksdb::Status::OK();
  if (count > static_cast<int>(metadata.size)) count = metadata.size;

  std::string score_bytes;
  double score = min ? kMinScore : kMaxScore;
  PutDouble(&score_bytes, score);
  std::string start_key, prefix_key, next_verison_prefix_key;
  InternalKey(ns_key, score_bytes, metadata.version, storage_->IsSlotIdEncoded()).Encode(&start_key);
  InternalKey(ns_key, "", metadata.version, storage_->IsSlotIdEncoded()).Encode(&prefix_key);
  InternalKey(ns_key, "", metadata.version + 1, storage_->IsSlotIdEncoded()).Encode(&next_verison_prefix_key);

  rocksdb::WriteBatch batch;
  WriteBatchLogData log_data(kRedisZSet);
  batch.PutLogData(log_data.Encode());

  rocksdb::ReadOptions read_options;
  LatestSnapShot ss(db_);
  read_options.snapshot = ss.GetSnapShot();
  rocksdb::Slice upper_bound(next_verison_prefix_key);
  read_options.iterate_upper_bound = &upper_bound;
  rocksdb::Slice lower_bound(prefix_key);
  read_options.iterate_lower_bound = &lower_bound;
  read_options.fill_cache = false;

  auto iter = db_->NewIterator(read_options, score_cf_handle_);
  iter->Seek(start_key);
  // see comment in rangebyscore()
  if (!min && (!iter->Valid() || !iter->key().starts_with(prefix_key))) {
    iter->SeekForPrev(start_key);
  }
  for (;
      iter->Valid() && iter->key().starts_with(prefix_key);
      min ? iter->Next() : iter->Prev()) {
    InternalKey ikey(iter->key(), storage_->IsSlotIdEncoded());
    Slice score_key = ikey.GetSubKey();
    GetDouble(&score_key, &score);
    mscores->emplace_back(MemberScore{score_key.ToString(), score});
    std::string default_cf_key;
    InternalKey(ns_key, score_key, metadata.version, storage_->IsSlotIdEncoded()).Encode(&default_cf_key);
    batch.Delete(default_cf_key);
    batch.Delete(score_cf_handle_, iter->key());
    if (mscores->size() >= static_cast<unsigned>(count)) break;
  }
  delete iter;

  if (!mscores->empty()) {
    metadata.size -= mscores->size();
    std::string bytes;
    metadata.Encode(&bytes);
    batch.Put(metadata_cf_handle_, ns_key, bytes);
  }
  return storage_->Write(rocksdb::WriteOptions(), &batch);
}

/* Redis Zrange 返回有序集中，指定区间内的成员。 

memberkey=ns+userkey+memberkeytype+member
方便rangebyindex-score  read_options.iterate_upper_bound read_options.iterate_lower_bound iter->Seek(start_key);
scorekey=ns+userkey+scoretype+score+member

其中成员的位置按分数值递增(从小到大)来排序。
具有相同分数值的成员按字典序(lexicographical order )来排列。
如果你需要成员按
值递减(从大到小)来排列，请使用 ZREVRANGE 命令。
下标参数 start 和 stop 都以 0 为底，也就是说，以 0 表示有序集第一个成员，以 1 表示有序集第二个成员，以此类推。
你也可以使用负数下标，以 -1 表示最后一个成员， -2 表示倒数第二个成员，以此类推。 */
rocksdb::Status ZSet::Range(const Slice &user_key, int start, int stop, uint8_t flags, std::vector<MemberScore>
*mscores) {
  mscores->clear();

  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  bool removed = (flags & (uint8_t)ZSET_REMOVED) != 0;
  bool reversed = (flags & (uint8_t)ZSET_REVERSED) != 0;

  std::unique_ptr<LockGuard> lock_guard;
  if (removed) lock_guard = std::unique_ptr<LockGuard>(new LockGuard(storage_->GetLockManager(), ns_key));
  ZSetMetadata metadata(false);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s.IsNotFound()? rocksdb::Status::OK():s;
  if (start < 0) start += metadata.size;
  if (stop < 0) stop += metadata.size;
  if (start < 0) start = 0;
  if (stop < 0 || start > stop) {
    return rocksdb::Status::OK();
  }

  std::string score_bytes;
  double score = !reversed ? kMinScore : kMaxScore;
  PutDouble(&score_bytes, score);
  std::string start_key, prefix_key, next_verison_prefix_key; //bound=userkey+socretype+socre+member
  InternalKey(ns_key, score_bytes, metadata.version, storage_->IsSlotIdEncoded()).Encode(&start_key);
  InternalKey(ns_key, "", metadata.version, storage_->IsSlotIdEncoded()).Encode(&prefix_key);
  InternalKey(ns_key, "", metadata.version + 1, storage_->IsSlotIdEncoded()).Encode(&next_verison_prefix_key);

  int count = 0;
  int removed_subkey = 0;
  rocksdb::ReadOptions read_options;
  LatestSnapShot ss(db_);
  read_options.snapshot = ss.GetSnapShot();
  rocksdb::Slice upper_bound(next_verison_prefix_key);
  read_options.iterate_upper_bound = &upper_bound;
  rocksdb::Slice lower_bound(prefix_key);
  read_options.iterate_lower_bound = &lower_bound;
  read_options.fill_cache = false;

  rocksdb::WriteBatch batch;
  auto iter = db_->NewIterator(read_options, score_cf_handle_);
  iter->Seek(start_key);
  // see comment in rangebyscore()
  if (reversed && (!iter->Valid() || !iter->key().starts_with(prefix_key))) {
    iter->SeekForPrev(start_key);
  }

  for (;
      iter->Valid() && iter->key().starts_with(prefix_key);
      !reversed ? iter->Next() : iter->Prev()) {
    InternalKey ikey(iter->key(), storage_->IsSlotIdEncoded());
    Slice score_key = ikey.GetSubKey();
    GetDouble(&score_key, &score);//提取score_key
    if (count >= start) {  //idx 先提取清单；后批量删除
      if (removed) {
        std::string sub_key;
        InternalKey(ns_key, score_key, metadata.version, storage_->IsSlotIdEncoded()).Encode(&sub_key);
        batch.Delete(sub_key);
        batch.Delete(score_cf_handle_, iter->key());
        removed_subkey++;
      }
      mscores->emplace_back(MemberScore{score_key.ToString(), score});
    }
    if (count++ >= stop) break;
  }
  delete iter;

  if (removed_subkey) {
    metadata.size -= removed_subkey;
    std::string bytes;
    metadata.Encode(&bytes);
    batch.Put(metadata_cf_handle_, ns_key, bytes);
    return storage_->Write(rocksdb::WriteOptions(), &batch);
  }
  return rocksdb::Status::OK();
}

//按分数，处理负分数
rocksdb::Status ZSet::RangeByScore(const Slice &user_key,
                                        ZRangeSpec spec,
                                        std::vector<MemberScore> *mscores,
                                        int *size) {
  if (size) *size = 0;
  if (mscores) mscores->clear();

  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  std::unique_ptr<LockGuard> lock_guard;
  if (spec.removed) lock_guard = std::unique_ptr<LockGuard>(new LockGuard(storage_->GetLockManager(), ns_key));
  ZSetMetadata metadata(false);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s.IsNotFound()? rocksdb::Status::OK():s;

  // let's get familiar with score first:
  //    a. score of zset's member is represented by double and it takes up 8 bytes in rocksdb
  //    b. to make positive double greater than native double in lexicographical order, score
  //       is required encoding before stored in rocksdb. encoding details see PutDouble()
  //    c. for convenience, user_score and inner_score respectively represent before and after encoding
  ////让我们先熟悉一下score：
//a.zset成员的分数用double表示，在rocksdb中占8字节
//b.在词典编纂顺序上，使正双精度大于本机双精度，分数存储到rocksdb之前需要进行编码。编码详细信息请参见PutDouble（）
//c.为方便起见，用户评分和内部评分分别代表编码前后
//
  // next lexicographical ordered inner_score of max:
  //    a. we can think of inner_score as a fixed 8-byte string. logically, the next lexicographical
  //       ordered inner_score of max_inner_score is 'max_inner_score + 1' if we assume no overflow.
  //       'max_inner_score + 1' means binary increment.
  //    b. realize binary increment 'max_inner_score + 1'
  //       use PutDouble() encoding max(max_user_score) to max_inner_score
  //       memcpy max_inner_score to u64(uint64_t)
  //       incr u64
  //       memcpy u64 to max_next_inner_score
  //    it may not be hard to understand about how to get max_next_inner_score

  ////下一个按字典顺序排列的最大内_分数：
//答：我们可以将内部_分数视为一个固定的8字节字符串。从逻辑上讲，下一个词典
//如果我们假设没有溢出，则最大内部分数的有序内部分数为“最大内部分数+1”。
//“最大内部分数+1”表示二进制增量。
//b.实现二进制增量“最大内部分数+1”
//使用PutDouble（）编码max（max_user_score）到max_internal_score
//memcpy最高内部分数为u64（uint64）
//incr u64
//memcpy u64至最高下一个内部分数
//也许不难理解如何获得max_next_inner_分数
//
  // directly generate max_next_user_score of max_next_inner_score:
  //    a. give a key argument first:
  //       for positive score, user_score is positively correlated with inner_score in lexicographical order
  //       for negative score, user_score is negatively correlated with inner_score in lexicographical order
  //       more details see PutDouble()
  //    b. get max_next_user_score of max_next_inner_score:
  //       for positive max_user_score, max_next_user_score is 'max_user_score + 1'
  //       for negative max_user_score, max_next_user_score is 'max_user_score - 1'
  // Note: fortunately, there is no overflow in fact. more details see binary encoding of double
  // binary encoding of double: https://en.wikipedia.org/wiki/Double-precision_floating-point_format
// //直接生成max_next_internal_score的max_next_user_分数：
//a.首先给出一个关键论点：
//对于正分数，用户分数与内部分数在字典顺序上呈正相关
//对于负面分数，用户分数与内部分数按字典顺序呈负相关
//更多详细信息请参见PutDouble（）
//b.获取max_next_用户的max_next_internal_分数：
//对于正的max_user_分数，max_next_user_分数为“max_user_分数+1”
//对于负的max_user_分数，max_next_user_分数为“max_user_分数-1”
//注：幸运的是，实际上没有溢出。更多详细信息请参见双精度二进制编码
//双精度二进制编码：https://en.wikipedia.org/wiki/Double-precision_floating-point_format

  // generate next possible score of max
  int64_t i64 = 0;
  double max_next_score = 0;
  if (spec.reversed && !spec.maxex) {
      memcpy(&i64, &spec.max, sizeof(spec.max));
      i64 = i64 >= 0 ? i64 + 1 : i64 - 1;
      memcpy(&max_next_score, &i64, sizeof(i64));
  }

  std::string start_score_bytes;
  PutDouble(&start_score_bytes, spec.reversed ? (spec.maxex ? spec.max : max_next_score) : spec.min);
  std::string start_key, prefix_key, next_verison_prefix_key;
  InternalKey(ns_key, start_score_bytes, metadata.version, storage_->IsSlotIdEncoded()).Encode(&start_key);
  InternalKey(ns_key, "", metadata.version, storage_->IsSlotIdEncoded()).Encode(&prefix_key);
  InternalKey(ns_key, "", metadata.version + 1, storage_->IsSlotIdEncoded()).Encode(&next_verison_prefix_key);

  rocksdb::ReadOptions read_options;
  LatestSnapShot ss(db_);
  read_options.snapshot = ss.GetSnapShot();
  rocksdb::Slice upper_bound(next_verison_prefix_key);//upper_bound
  read_options.iterate_upper_bound = &upper_bound;
  rocksdb::Slice lower_bound(prefix_key);
  read_options.iterate_lower_bound = &lower_bound;//lower_bound
  read_options.fill_cache = false;

  int pos = 0;
  auto iter = db_->NewIterator(read_options, score_cf_handle_);
  rocksdb::WriteBatch batch;
  WriteBatchLogData log_data(kRedisZSet);
  batch.PutLogData(log_data.Encode());

  if (!spec.reversed) {
    iter->Seek(start_key);
  } else {
    iter->SeekForPrev(start_key);
    if (iter->Valid() && iter->key().starts_with(start_key)) {
      iter->Prev();
    }
  }

  for (;
      iter->Valid() && iter->key().starts_with(prefix_key);
      !spec.reversed ? iter->Next() : iter->Prev()) {
    InternalKey ikey(iter->key(), storage_->IsSlotIdEncoded());
    Slice score_key = ikey.GetSubKey();
    double score;
    GetDouble(&score_key, &score);
    if (spec.reversed) {
      if ((spec.minex && score == spec.min) || score < spec.min) break;
      if ((spec.maxex && score == spec.max) || score > spec.max) continue;
    } else {
      if ((spec.minex && score == spec.min) || score < spec.min) continue;
      if ((spec.maxex && score == spec.max) || score > spec.max) break;
    }
    if (spec.offset >= 0 && pos++ < spec.offset) continue;
    if (spec.removed) {
      std::string sub_key;
      InternalKey(ns_key, score_key, metadata.version, storage_->IsSlotIdEncoded()).Encode(&sub_key);
      batch.Delete(sub_key);
      batch.Delete(score_cf_handle_, iter->key());
    } else {
      if (mscores) mscores->emplace_back(MemberScore{score_key.ToString(), score});
    }
    if (size) *size += 1;
    if (spec.count > 0 && mscores && mscores->size() >= static_cast<unsigned>(spec.count)) break;
  }
  delete iter;

  if (spec.removed && *size > 0) {
    metadata.size -= *size;
    std::string bytes;
    metadata.Encode(&bytes);
    batch.Put(metadata_cf_handle_, ns_key, bytes);
    return storage_->Write(rocksdb::WriteOptions(), &batch);
  }
  return rocksdb::Status::OK();
}

//按字典member排序范围获取清单；
rocksdb::Status ZSet::RangeByLex(const Slice &user_key,
                                 ZRangeLexSpec spec,
                                 std::vector<std::string> *members,
                                 int *size) {
  if (size) *size = 0;
  if (members) members->clear();
  //页尾
  if (spec.offset > -1 && spec.count == 0) {
      return rocksdb::Status::OK();
  }

  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  ZSetMetadata metadata(false);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s.IsNotFound() ? rocksdb::Status::OK() : s;

//start_member reversed-对结果反向排序 version-membertype
  std::string start_member = spec.reversed ? spec.max : spec.min;
  std::string start_key, prefix_key, next_version_prefix_key;
  InternalKey(ns_key, start_member, metadata.version, storage_->IsSlotIdEncoded()).Encode(&start_key);
  InternalKey(ns_key, "", metadata.version, storage_->IsSlotIdEncoded()).Encode(&prefix_key);
  InternalKey(ns_key, "", metadata.version + 1, storage_->IsSlotIdEncoded()).Encode(&next_version_prefix_key);

// 查询范围
  rocksdb::ReadOptions read_options;
  LatestSnapShot ss(db_);
  read_options.snapshot = ss.GetSnapShot();
  rocksdb::Slice upper_bound(next_version_prefix_key);
  read_options.iterate_upper_bound = &upper_bound;
  rocksdb::Slice lower_bound(prefix_key);
  read_options.iterate_lower_bound = &lower_bound;
  read_options.fill_cache = false;

  int pos = 0;
  auto iter = db_->NewIterator(read_options);
  rocksdb::WriteBatch batch;
  WriteBatchLogData log_data(kRedisZSet);
  batch.PutLogData(log_data.Encode());

  if (!spec.reversed) {
    iter->Seek(start_key);
  } else {
    if (spec.max_infinite) {//?
      iter->SeekToLast();
    } else {
      iter->SeekForPrev(start_key);
    }
  }

  for (;
       iter->Valid() && iter->key().starts_with(prefix_key);
       //next prev
       (!spec.reversed ? iter->Next() : iter->Prev())) {
    InternalKey ikey(iter->key(), storage_->IsSlotIdEncoded());
    Slice member = ikey.GetSubKey();
    if (spec.reversed) {//反转
        if (member.ToString() < spec.min || (spec.minex && member == spec.min)) {
            break;
        }
        if ((spec.maxex && member == spec.max) || (!spec.max_infinite && member.ToString() > spec.max)) {
            continue;
        }
    } else {//非反转
       if (spec.minex && member == spec.min) continue;  // the min member was exclusive
       if ((spec.maxex && member == spec.max) || (!spec.max_infinite && member.ToString() > spec.max)) break;
    }

    //分页
    if (spec.offset >= 0 && pos++ < spec.offset) continue;
    //删除
    if (spec.removed) {
      std::string score_bytes = iter->value().ToString();
      score_bytes.append(member.ToString());
      std::string score_key;
      InternalKey(ns_key, score_bytes, metadata.version, storage_->IsSlotIdEncoded()).Encode(&score_key);
      batch.Delete(score_cf_handle_, score_key);
      batch.Delete(iter->key());
    } else {
      if (members) members->emplace_back(member.ToString());
    }
    if (size) *size += 1;
    if (spec.count > 0 && members && members->size() >= static_cast<unsigned>(spec.count)) break;
  }
  delete iter;

  if (spec.removed && *size > 0) {
    metadata.size -= *size;
    std::string bytes;
    metadata.Encode(&bytes);
    batch.Put(metadata_cf_handle_, ns_key, bytes);
    return storage_->Write(rocksdb::WriteOptions(), &batch);
  }
  return rocksdb::Status::OK();
}
//get
rocksdb::Status ZSet::Score(const Slice &user_key, const Slice &member, double *score) {
  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);
  ZSetMetadata metadata(false);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s;

  rocksdb::ReadOptions read_options;
  LatestSnapShot ss(db_);
  read_options.snapshot = ss.GetSnapShot();

  std::string member_key, score_bytes;
  InternalKey(ns_key, member, metadata.version, storage_->IsSlotIdEncoded()).Encode(&member_key);
  s = db_->Get(read_options, member_key, &score_bytes);
  if (!s.ok()) return s;
  *score = DecodeDouble(score_bytes.data());
  return rocksdb::Status::OK();
}

rocksdb::Status ZSet::Remove(const Slice &user_key, const std::vector<Slice> &members, int *ret) {
  *ret = 0;
  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);
  ZSetMetadata metadata(false);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s.IsNotFound()? rocksdb::Status::OK():s;

  rocksdb::WriteBatch batch;
  WriteBatchLogData log_data(kRedisZSet);
  batch.PutLogData(log_data.Encode());
  int removed = 0;
  std::string member_key, score_key;
  for (const auto &member : members) {
    InternalKey(ns_key, member, metadata.version, storage_->IsSlotIdEncoded()).Encode(&member_key);
    std::string score_bytes;
    s = db_->Get(rocksdb::ReadOptions(), member_key, &score_bytes);
    if (s.ok()) {
      score_bytes.append(member.ToString());
      InternalKey(ns_key, score_bytes, metadata.version, storage_->IsSlotIdEncoded()).Encode(&score_key);
      batch.Delete(member_key); //member_key merge-put null->return value
      batch.Delete(score_cf_handle_, score_key);//score_key
      removed++;
    }
  }
  //meta size
  if (removed > 0) {
    *ret = removed;
    metadata.size -= removed;
    std::string bytes;
    metadata.Encode(&bytes);
    batch.Put(metadata_cf_handle_, ns_key, bytes);
  }
  return storage_->Write(rocksdb::WriteOptions(), &batch);
}

rocksdb::Status ZSet::RemoveRangeByScore(const Slice &user_key, ZRangeSpec spec, int *ret) {
  spec.removed = true;
  return RangeByScore(user_key, spec, nullptr, ret);
}

rocksdb::Status ZSet::RemoveRangeByLex(const Slice &user_key, ZRangeLexSpec spec, int *ret) {
  spec.removed = true;
  return RangeByLex(user_key, spec, nullptr, ret);
}

rocksdb::Status ZSet::RemoveRangeByRank(const Slice &user_key, int start, int stop, int *ret) {
  uint8_t flags = ZSET_REMOVED;
  std::vector<MemberScore> mscores;
  rocksdb::Status s = Range(user_key, start, stop, flags, &mscores);
  *ret = static_cast<int>(mscores.size());
  return s;
}

rocksdb::Status ZSet::Rank(const Slice &user_key, const Slice &member, bool reversed, int *ret) {
  *ret = -1;

  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);
  ZSetMetadata metadata(false);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s.IsNotFound()? rocksdb::Status::OK():s;

  rocksdb::ReadOptions read_options;
  LatestSnapShot ss(db_);
  read_options.snapshot = ss.GetSnapShot();
  std::string score_bytes, member_key;
  InternalKey(ns_key, member, metadata.version, storage_->IsSlotIdEncoded()).Encode(&member_key);
  s = db_->Get(read_options, member_key, &score_bytes);
  if (!s.ok()) return s.IsNotFound()? rocksdb::Status::OK():s;

  double target_score = DecodeDouble(score_bytes.data());
  std::string start_score_bytes, start_key, prefix_key, next_verison_prefix_key;
  double start_score = !reversed ? kMinScore : kMaxScore;
  PutDouble(&start_score_bytes, start_score);
  InternalKey(ns_key, start_score_bytes, metadata.version, storage_->IsSlotIdEncoded()).Encode(&start_key);
  InternalKey(ns_key, "", metadata.version, storage_->IsSlotIdEncoded()).Encode(&prefix_key);
  InternalKey(ns_key, "", metadata.version + 1, storage_->IsSlotIdEncoded()).Encode(&next_verison_prefix_key);

  int rank = 0;
  rocksdb::Slice upper_bound(next_verison_prefix_key);
  read_options.iterate_upper_bound = &upper_bound;
  rocksdb::Slice lower_bound(prefix_key);
  read_options.iterate_lower_bound = &lower_bound;
  read_options.fill_cache = false;

  auto iter = db_->NewIterator(read_options, score_cf_handle_);
  iter->Seek(start_key);
  // see comment in rangebyscore()
  if (reversed && (!iter->Valid() || !iter->key().starts_with(prefix_key))) {
    iter->SeekForPrev(start_key);
  }
  for (;
      iter->Valid() && iter->key().starts_with(prefix_key);
      !reversed ? iter->Next() : iter->Prev()) {
    InternalKey ikey(iter->key(), storage_->IsSlotIdEncoded());
    Slice score_key = ikey.GetSubKey();
    double score;
    GetDouble(&score_key, &score);
    if (score == target_score && score_key == member) break;
    rank++;
  }
  delete iter;

  *ret = rank;
  return rocksdb::Status::OK();
}

rocksdb::Status ZSet::Overwrite(const Slice &user_key, const std::vector<MemberScore> &mscores) {
  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);
  ZSetMetadata metadata;
  rocksdb::WriteBatch batch;
  WriteBatchLogData log_data(kRedisZSet);
  batch.PutLogData(log_data.Encode());
  for (const auto &ms : mscores) {
    std::string member_key, score_bytes, score_key;
    InternalKey(ns_key, ms.member, metadata.version, storage_->IsSlotIdEncoded()).Encode(&member_key);
    PutDouble(&score_bytes, ms.score);
    batch.Put(member_key, score_bytes);
    score_bytes.append(ms.member);
    InternalKey(ns_key, score_bytes, metadata.version, storage_->IsSlotIdEncoded()).Encode(&score_key);
    batch.Put(score_cf_handle_, score_key, Slice());
  }
  metadata.size = static_cast<uint32_t>(mscores.size());
  std::string bytes;
  metadata.Encode(&bytes);
  batch.Put(metadata_cf_handle_, ns_key, bytes);
  return storage_->Write(rocksdb::WriteOptions(), &batch);
}

//交集
rocksdb::Status ZSet::InterStore(const Slice &dst,
                                 const std::vector<KeyWeight> &keys_weights,
                                 AggregateMethod aggregate_method,
                                 int *size) {
  if (size) *size = 0;

  std::map<std::string, double> dst_zset;
  std::map<std::string, size_t> member_counters;
  std::vector<MemberScore> target_mscores;
  int target_size;
  ZRangeSpec spec;
  auto s = RangeByScore(keys_weights[0].key, spec, &target_mscores, &target_size);
  if (!s.ok() || target_mscores.empty()) return s;
  for (const auto &ms : target_mscores) {
    double score = ms.score * keys_weights[0].weight;
    if (std::isnan(score)) score = 0;
    dst_zset[ms.member] = score;
    member_counters[ms.member] = 1;
  }
  for (size_t i = 1; i < keys_weights.size(); i++) {
    auto s = RangeByScore(keys_weights[i].key, spec, &target_mscores, &target_size);
    if (!s.ok() || target_mscores.empty()) return s;
    for (const auto &ms : target_mscores) {
      if (dst_zset.find(ms.member) == dst_zset.end()) continue;
      member_counters[ms.member]++;
      double score = ms.score * keys_weights[i].weight;
      if (std::isnan(score)) score = 0;
      switch (aggregate_method) {
        case kAggregateSum:
          dst_zset[ms.member] += score;
          if (std::isnan(dst_zset[ms.member])) {
            dst_zset[ms.member] = 0;
          }
          break;
        case kAggregateMin:
          if (dst_zset[ms.member] > score) {
            dst_zset[ms.member] = score;
          }
          break;
        case kAggregateMax:
          if (dst_zset[ms.member] < score) {
            dst_zset[ms.member] = score;
          }
          break;
      }
    }
  }
  if (!dst_zset.empty()) {
    std::vector<MemberScore> mscores;
    for (const auto &iter : dst_zset) {
      if (member_counters[iter.first] != keys_weights.size()) continue;
      mscores.emplace_back(MemberScore{iter.first, iter.second});
    }
    if (size) *size = mscores.size();
    Overwrite(dst, mscores);
  }

  return rocksdb::Status::OK();
}

//并集
rocksdb::Status ZSet::UnionStore(const Slice &dst,
                                 const std::vector<KeyWeight> &keys_weights,
                                 AggregateMethod aggregate_method,
                                 int *size) {
  if (size) *size = 0;

  std::map<std::string, double> dst_zset;
  std::vector<MemberScore> target_mscores;
  int target_size;
  ZRangeSpec spec;
  for (const auto &key_weight : keys_weights) {
    // get all member
    auto s = RangeByScore(key_weight.key, spec, &target_mscores, &target_size);
    if (!s.ok() && !s.IsNotFound()) return s;
    for (const auto &ms : target_mscores) {
      double score = ms.score * key_weight.weight;
      if (std::isnan(score)) score = 0;
      if (dst_zset.find(ms.member) == dst_zset.end()) {
        dst_zset[ms.member] = score;
      } else {
        switch (aggregate_method) {
          case kAggregateSum:
            dst_zset[ms.member] += score;
            if (std::isnan(dst_zset[ms.member]))
              dst_zset[ms.member] = 0;
            break;
          case kAggregateMin:
            if (dst_zset[ms.member] > score) {
              dst_zset[ms.member] = score;
            }
            break;
          case kAggregateMax:
            if (dst_zset[ms.member] < score) {
              dst_zset[ms.member] = score;
            }
            break;
        }
      }
    }
  }
  if (!dst_zset.empty()) {
    std::vector<MemberScore> mscores;
    for (const auto &iter : dst_zset) {
      mscores.emplace_back(MemberScore{iter.first, iter.second});
    }
    if (size) *size = mscores.size();
    Overwrite(dst, mscores);
  }

  return rocksdb::Status::OK();
}

//协议处理
Status ZSet::ParseRangeSpec(const std::string &min, const std::string &max, ZRangeSpec *spec) {
  const char *sptr = nullptr;
  char *eptr = nullptr;

  if (min == "+inf" ||  max == "-inf") {
    return Status(Status::NotOK, "min > max");
  }

  if (min == "-inf") {
    spec->min = kMinScore;
  } else {
    sptr = min.data();
    if (!min.empty() && min[0] == '(') {
      spec->minex = true;
      sptr++;
    }
    spec->min = strtod(sptr, &eptr);
    if ((eptr && eptr[0] != '\0') || isnan(spec->min)) {
      return Status(Status::NotOK, "the min isn't double");
    }
  }

  if (max == "+inf") {
    spec->max = kMaxScore;
  } else {
    sptr = max.data();
    if (!max.empty() && max[0] == '(') {
      spec->maxex = true;
      sptr++;
    }
    spec->max = strtod(sptr, &eptr);
    if ((eptr && eptr[0] != '\0') || isnan(spec->max)) {
      return Status(Status::NotOK, "the max isn't double");
    }
  }
  return Status::OK();
}

Status ZSet::ParseRangeLexSpec(const std::string &min, const std::string &max, ZRangeLexSpec *spec) {
  if (min == "+" || max == "-") {
    return Status(Status::NotOK, "min > max");
  }

  if (min == "-") {
    spec->min = "";
  } else {
    if (min[0] == '(') {
      spec->minex = true;
    } else if (min[0] == '[') {
      spec->minex = false;
    } else {
      return Status(Status::NotOK, "the min is illegal");
    }
    spec->min = min.substr(1);
  }

  if (max == "+") {
    spec->max_infinite = true;
  } else {
    if (max[0] == '(') {
      spec->maxex = true;
    } else if (max[0] == '[') {
      spec->maxex = false;
    } else {
      return Status(Status::NotOK, "the max is illegal");
    }
    spec->max = max.substr(1);
  }
  return Status::OK();
}

rocksdb::Status ZSet::Scan(const Slice &user_key,
                           const std::string &cursor,
                           uint64_t limit,
                           const std::string &member_prefix,
                           std::vector<std::string> *members,
                           std::vector<double> *scores) {
  if (scores != nullptr) {
    std::vector<std::string> values;
    auto s = SubKeyScanner::Scan(kRedisZSet, user_key, cursor, limit, member_prefix, members, &values);
    if (!s.ok()) return s;

    for (const auto &value : values) {
      double target_score = DecodeDouble(value.data());
      scores->emplace_back(target_score);
    }
    return s;
  }
  return SubKeyScanner::Scan(kRedisZSet, user_key, cursor, limit, member_prefix, members);
}

rocksdb::Status ZSet::MGet(const Slice &user_key,
                           const std::vector<Slice> &members,
                           std::map<std::string, double> *mscores) {
  mscores->clear();

  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);
  ZSetMetadata metadata(false);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s;

  rocksdb::ReadOptions read_options;
  LatestSnapShot ss(db_);
  read_options.snapshot = ss.GetSnapShot();
  std::string score_bytes, member_key;
  for (const auto &member : members) {
    InternalKey(ns_key, member, metadata.version, storage_->IsSlotIdEncoded()).Encode(&member_key);
    score_bytes.clear();
    s = db_->Get(read_options, member_key, &score_bytes);
    if (!s.ok() && !s.IsNotFound()) return s;
    if (s.IsNotFound()) {
      continue;
    }
    double target_score = DecodeDouble(score_bytes.data());
    (*mscores)[member.ToString()] = target_score;
  }
  return rocksdb::Status::OK();
}

}  // namespace Redis
