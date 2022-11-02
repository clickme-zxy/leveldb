// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/memtable.h"
#include "db/dbformat.h"
#include "leveldb/comparator.h"
#include "leveldb/env.h"
#include "leveldb/iterator.h"
#include "util/coding.h"

namespace leveldb {

// 得到共同长度前缀的length
static Slice GetLengthPrefixedSlice(const char* data) {
  uint32_t len;
  const char* p = data;
  p = GetVarint32Ptr(p, p + 5, &len);  // +5: we assume "p" is not corrupted
  // 将data构造成符合memtable的共同前缀的p
  return Slice(p, len);
}

// 根据内部，构造了我自己的comparator
// 为啥不顺便构造一下我的iterator
MemTable::MemTable(const InternalKeyComparator& comparator)
    : comparator_(comparator), refs_(0), table_(comparator_, &arena_) {}

MemTable::~MemTable() { assert(refs_ == 0); }

// 计算使用的内存
size_t MemTable::ApproximateMemoryUsage() { return arena_.MemoryUsage(); }

// 比较的内容是char,也就是外面传入的string?
// 而不是我们的slice
// 一个对internalKeyCompartor的包装
int MemTable::KeyComparator::operator()(const char* aptr,
                                        const char* bptr) const {
  // Internal keys are encoded as length-prefixed strings.
  Slice a = GetLengthPrefixedSlice(aptr);
  Slice b = GetLengthPrefixedSlice(bptr);
  return comparator.Compare(a, b);
}

// Encode a suitable internal key target for "target" and return it.
// Uses *scratch as scratch space, and the returned pointer will point
// into this scratch space.
// 将slice转换为对应的string
static const char* EncodeKey(std::string* scratch, const Slice& target) {
  scratch->clear();
  PutVarint32(scratch, target.size());
  scratch->append(target.data(), target.size());
  return scratch->data();
}

// 构建我之前声明的iterator
// 好繁琐的逻辑
// 一层嵌套一层
class MemTableIterator : public Iterator {
 public:
  // 专门根据跳表构建的iterator
  explicit MemTableIterator(MemTable::Table* table) : iter_(table) {}
  private:
  MemTable::Table::Iterator iter_;

  MemTableIterator(const MemTableIterator&) = delete;
  MemTableIterator& operator=(const MemTableIterator&) = delete;

  ~MemTableIterator() override = default;

  bool Valid() const override { return iter_.Valid(); }
  void Seek(const Slice& k) override { iter_.Seek(EncodeKey(&tmp_, k)); }
  void SeekToFirst() override { iter_.SeekToFirst(); }
  void SeekToLast() override { iter_.SeekToLast(); }
  void Next() override { iter_.Next(); }
  void Prev() override { iter_.Prev(); }
  Slice key() const override { return GetLengthPrefixedSlice(iter_.key()); }
  Slice value() const override {
    Slice key_slice = GetLengthPrefixedSlice(iter_.key());
    return GetLengthPrefixedSlice(key_slice.data() + key_slice.size());
  }

  Status status() const override { return Status::OK(); }

 private:
  MemTable::Table::Iterator iter_;
  std::string tmp_;  // For passing to EncodeKey
};

// 构造了table的iterator
Iterator* MemTable::NewIterator() { return new MemTableIterator(&table_); }

// 动态长度的加入、
// 每个长度都不一样，之后解读会存在问题吗
void MemTable::Add(SequenceNumber s, ValueType type, const Slice& key,
                   const Slice& value) {
  // Format of an entry is concatenation of:
  //  key_size     : varint32 of internal_key.size()
  //  key bytes    : char[internal_key.size()]
  //  tag          : uint64((sequence << 8) | type)---8个B 
  //  value_size   : varint32 of value.size()
  //  value bytes  : char[value.size()]
  size_t key_size = key.size();
  size_t val_size = value.size();
  // 多出了2B
  size_t internal_key_size = key_size + 8;
  const size_t encoded_len = VarintLength(internal_key_size) +
                             internal_key_size + VarintLength(val_size) +
                             val_size;
  // 计算出需要分配空间的大小
  char* buf = arena_.Allocate(encoded_len);
  // encoded_len  internal_key_size(长度)+internal_key_size+val_size(长度)+val_size(动态长度)
  // 指向internal_key_size大小之后的p
  char* p = EncodeVarint32(buf, internal_key_size);
  // 放入key的相关内容
  // var(internal)+key.data()
  std::memcpy(p, key.data(), key_size);
  p += key_size;
  // 还有多余的2B
  // 多余的2B 放入我们的tag
  EncodeFixed64(p, (s << 8) | type);
  p += 8;
  p = EncodeVarint32(p, val_size);
  std::memcpy(p, value.data(), val_size);
  // 我的长度要全部填满
  assert(p + val_size == buf + encoded_len);
  // 将buf插入我的table_中
  table_.Insert(buf);
}

// lookup key由三个部分组成
// internal_key 就是key+type
// memtable_key 全部的信息?
// user_key 就是key
// 传入key的类型 是look_up，为什么不是slice ？ 
// 解答，和key的组成有关,ADD的内容是一个全部信息的memtable_key之后组成LOOKIUPKEY对相关信息进行解码
// 利用LOOKUPKEY 进行对internal_key中的key部分的解码，然后对里面的tag进行解读，从而实现对value的阅读
bool MemTable::Get(const LookupKey& key, std::string* value, Status* s) {
  // 将look_up转成slice
  Slice memkey = key.memtable_key();
  // 这是遍历跳表的iterator
  Table::Iterator iter(&table_);
  // 很明显，是一个封装
  // 在跳表中，查找结构
  iter.Seek(memkey.data());
  // 找到之后，key和实际的内容进行逐1的验证，验证key一样之后，进行解读
  if (iter.Valid()) {
    // entry format is:
    //    klength  varint32
    //    userkey  char[klength]
    //    tag      uint64
    //    vlength  varint32
    //    value    char[vlength]
    // Check that it belongs to same user key.  We do not check the
    // sequence number since the Seek() call above should have skipped
    // all entries with overly large sequence numbers.
    const char* entry = iter.key();
    uint32_t key_length;
    // 确实得到了一个4B的信息，得到了目前可变的长度是多少，以及返回了开头指针
    const char* key_ptr = GetVarint32Ptr(entry, entry + 5, &key_length);
    // 比较user_key的基本信息
    if (comparator_.comparator.user_comparator()->Compare(
            Slice(key_ptr, key_length - 8), key.user_key()) == 0) {
      // Correct user key
      const uint64_t tag = DecodeFixed64(key_ptr + key_length - 8);
      switch (static_cast<ValueType>(tag & 0xff)) {
        case kTypeValue: {
          Slice v = GetLengthPrefixedSlice(key_ptr + key_length);
          value->assign(v.data(), v.size());
          return true;
        }
        case kTypeDeletion:
          *s = Status::NotFound(Slice());
          return true;
      }
    }
  }
  return false;
}

}  // namespace leveldb
