#include "redis_metadata.h"
#include "redis_slot.h"
#include <time.h>
#include <stdlib.h>
#include <sys/time.h>

#include <vector>
#include <atomic>
#include <rocksdb/env.h>

// 52 bit for microseconds and 11 bit for counter
const int VersionCounterBits = 11;

static std::atomic<uint64_t> version_counter_ = {0};

InternalKey::InternalKey(Slice input, bool slot_id_encoded) {
  slot_id_encoded_ = slot_id_encoded;
  uint32_t key_size;
  uint8_t namespace_size;
  GetFixed8(&input, &namespace_size);
  namespace_ = Slice(input.data(), namespace_size);
  input.remove_prefix(namespace_size);
  if (slot_id_encoded_) {
    GetFixed16(&input, &slotid_);
  }
  GetFixed32(&input, &key_size);
  key_ = Slice(input.data(), key_size);
  input.remove_prefix(key_size);
  GetFixed64(&input, &version_);
  sub_key_ = Slice(input.data(), input.size());
  buf_ = nullptr;
  memset(prealloc_, '\0', sizeof(prealloc_));
}

InternalKey::InternalKey(Slice ns_key, Slice sub_key, uint64_t version, bool slot_id_encoded) {
  slot_id_encoded_ = slot_id_encoded;
  uint8_t namespace_size;
  GetFixed8(&ns_key, &namespace_size);
  namespace_ = Slice(ns_key.data(), namespace_size);
  ns_key.remove_prefix(namespace_size);
  if (slot_id_encoded_) {
    GetFixed16(&ns_key, &slotid_);
  }
  key_ = ns_key;
  sub_key_ = sub_key;
  version_ = version;
  buf_ = nullptr;
  memset(prealloc_, '\0', sizeof(prealloc_));
}

InternalKey::~InternalKey() {
  if (buf_ != nullptr && buf_ != prealloc_) delete []buf_;
}

Slice InternalKey::GetNamespace() const {
  return namespace_;
}

Slice InternalKey::GetKey() const {
  return key_;
}

Slice InternalKey::GetSubKey() const {
  return sub_key_;
}

uint64_t InternalKey::GetVersion() const {
  return version_;
}

void InternalKey::Encode(std::string *out) {
  out->clear();
  size_t pos = 0;
  size_t total = 1+namespace_.size()+4+key_.size()+8+sub_key_.size();
  if (slot_id_encoded_) {
    total += 2;
  }
  if (total < sizeof(prealloc_)) {
    buf_ = prealloc_;
  } else {
    buf_ = new char[total];
  }
  EncodeFixed8(buf_+pos, static_cast<uint8_t>(namespace_.size()));
  pos += 1;
  memcpy(buf_+pos, namespace_.data(), namespace_.size());
  pos += namespace_.size();
  if (slot_id_encoded_) {
    EncodeFixed16(buf_+pos, slotid_);
    pos += 2;
  }
  EncodeFixed32(buf_+pos, static_cast<uint32_t>(key_.size()));
  pos += 4;
  memcpy(buf_+pos, key_.data(), key_.size());
  pos += key_.size();
  EncodeFixed64(buf_+pos, version_);
  pos += 8;
  memcpy(buf_+pos, sub_key_.data(), sub_key_.size());
  pos += sub_key_.size();
  out->assign(buf_, pos);
}

bool InternalKey::operator==(const InternalKey &that) const {
  if (key_ != that.key_) return false;
  if (sub_key_ != that.sub_key_) return false;
  return version_ == that.version_;
}

// 提取 ns key
void ExtractNamespaceKey(Slice ns_key, std::string *ns, std::string *key, bool slot_id_encoded) {
  uint8_t namespace_size;
  GetFixed8(&ns_key, &namespace_size);

  //todo str.slice(start,end)，截取str从start到end的所有字符（包含起始位置，不包含结束位置）
  *ns = ns_key.ToString().substr(0, namespace_size);
  ns_key.remove_prefix(namespace_size);

  if (slot_id_encoded) {
    uint16_t slot_id;
    GetFixed16(&ns_key, &slot_id);
  }

  *key = ns_key.ToString();
}
// 组合主键
void ComposeNamespaceKey(const Slice& ns, const Slice& key, std::string *ns_key, bool slot_id_encoded) {
  ns_key->clear();
  // ns_size+16bit slot_id+key
  // static_cast	用于良性转换，一般不会导致意外发生，风险很低。
  PutFixed8(ns_key, static_cast<uint8_t>(ns.size()));
  ns_key->append(ns.ToString());

  if (slot_id_encoded) {
    auto slot_id = GetSlotNumFromKey(key.ToString());
    PutFixed16(ns_key, slot_id);
  }

  ns_key->append(key.ToString());
}
// 组合prefix ns+slot
void ComposeSlotKeyPrefix(const Slice& ns, int slotid, std::string *output) {
  output->clear();

  PutFixed8(output, static_cast<uint8_t>(ns.size()));
  output->append(ns.ToString());

  PutFixed16(output, static_cast<uint16_t>(slotid));
}

Metadata::Metadata(RedisType type, bool generate_version) {
  flags = (uint8_t)0x0f & type;
  expire = 0;
  version = 0;
  size = 0;
  if (generate_version) version = generateVersion();
}

// 解码 metaval flag+expire+version+size
rocksdb::Status Metadata::Decode(const std::string &bytes) {
  // flags(1byte) + expire (4byte)
  if (bytes.size() < 5) {
    return rocksdb::Status::InvalidArgument("the metadata was too short");
  }
  Slice input(bytes);
  GetFixed8(&input, &flags);
  GetFixed32(&input, reinterpret_cast<uint32_t *>(&expire));
  if (Type() != kRedisString) {
    if (input.size() < 12) rocksdb::Status::InvalidArgument("the metadata was too short");
    GetFixed64(&input, &version);
    GetFixed32(&input, &size);
  }
  return rocksdb::Status::OK();
}

// 编码 metaval
void Metadata::Encode(std::string *dst) {
  PutFixed8(dst, flags);
  PutFixed32(dst, (uint32_t) expire);
  if (Type() != kRedisString) {
    PutFixed64(dst, version);
    PutFixed32(dst, size);
  }
}

void Metadata::InitVersionCounter() {
  struct timeval now;
  gettimeofday(&now, nullptr);
  // use random position for initial counter to avoid conflicts,
  // when the slave was promoted as master and the system clock may backoff
  srand(static_cast<unsigned>(now.tv_sec));
  version_counter_ = static_cast<uint64_t>(std::rand());
}

uint64_t Metadata::generateVersion() {
  struct timeval now;
  gettimeofday(&now, nullptr);
  uint64_t version = static_cast<uint64_t >(now.tv_sec)*1000000;
  version += static_cast<uint64_t>(now.tv_usec);
  uint64_t counter = version_counter_.fetch_add(1);
  return (version << VersionCounterBits) + (counter%(1 << VersionCounterBits));
}

// 操作符重载
bool Metadata::operator==(const Metadata &that) const {
  if (flags != that.flags) return false;
  if (expire != that.expire) return false;
  if (Type() != kRedisString) {
    if (size != that.size) return false;
    if (version != that.version) return false;
  }
  return true;
}

// 获取数据类型
RedisType Metadata::Type() const {
  return static_cast<RedisType>(flags & (uint8_t)0x0f);
}

// 有效时间 ttl 
int32_t Metadata::TTL() const {
  int64_t now;
  rocksdb::Env::Default()->GetCurrentTime(&now);
  if (expire != 0 && expire < now) {
    return -2;
  }
  return expire <= 0 ? -1 : int32_t (expire - now);
}

timeval Metadata::Time() const {
  auto t = version >> VersionCounterBits;
  struct timeval created_at{static_cast<uint32_t>(t / 1000000), static_cast<int32_t>(t % 1000000)};
  return created_at;
}

// 设置有效期 ?
bool Metadata::Expired() const {
  int64_t now;
  rocksdb::Env::Default()->GetCurrentTime(&now);
  if (expire > 0 && expire < now) {
    return true;
  }
  return Type() != kRedisString && size == 0;
}

// 使用UINT64_MAXfrom中的常量stdint.h并自己进行溢出预测。
ListMetadata::ListMetadata(bool generate_version) : Metadata(kRedisList, generate_version) {
  head = UINT64_MAX/2;
  tail = head;
}

//  meta-val 编码
void ListMetadata::Encode(std::string *dst) {
  Metadata::Encode(dst);
  PutFixed64(dst, head);
  PutFixed64(dst, tail);
}

rocksdb::Status ListMetadata::Decode(const std::string &bytes) {
  Slice input(bytes);
  GetFixed8(&input, &flags);
  GetFixed32(&input, reinterpret_cast<uint32_t *>(&expire));
  if (Type() != kRedisString) {
    if (input.size() < 12) rocksdb::Status::InvalidArgument("the metadata was too short");
    GetFixed64(&input, &version);
    GetFixed32(&input, &size);
  }
  if (Type() == kRedisList) {
    if (input.size() < 16) rocksdb::Status::InvalidArgument("the metadata was too short");
    GetFixed64(&input, &head);
    GetFixed64(&input, &tail);
  }
  return rocksdb::Status();
}
