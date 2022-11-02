// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_MEMTABLE_H_
#define STORAGE_LEVELDB_DB_MEMTABLE_H_

#include <string>

#include "db/dbformat.h"
#include "db/skiplist.h"
#include "leveldb/db.h"
#include "util/arena.h"

// memtable底层采用的是skiplist来进行实现的
namespace leveldb {

// 内部比较器
// memtable中的keyCompartor就是对InternalKeyCompartor的一个封装
class InternalKeyComparator;
// 内部迭代器
class MemTableIterator;
// 实际上对memtable 中的table的iter进行了封装
// 也就是利用跳表的iter来进行实现的

class MemTable {
 public:
  // explict 是一个防止形参转换为obejct隐式转换的一个参数
  // 在C++中，对于单个参数可以构造的object，可以将单个单数变成obejct，从而隐式的进行转换，采用explict可以防止这种隐式的转换
  // 例如 BOOK a= BOOK("AA");a.isSame(String("bb"))将String("bb")隐式的转为BOOK
  // MemTables are reference counted.  The initial reference count
  // is zero and the caller must call Ref() at least once.
  explicit MemTable(const InternalKeyComparator& comparator);

  // 除了这种构造方式的其他构造统一删除？
  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  // Increase reference count.
  void Ref() { ++refs_; }

  // Drop reference count.  Delete if no more references exist.
  void Unref() {
    --refs_;
    assert(refs_ >= 0);
    // 实际上就是==0
    if (refs_ <= 0) {
      delete this;
    }
  }

  // Returns an estimate of the number of bytes of data in use by this
  // data structure. It is safe to call when MemTable is being modified.
  size_t ApproximateMemoryUsage();

  // Return an iterator that yields the contents of the memtable.
  //
  // The caller must ensure that the underlying MemTable remains live
  // while the returned iterator is live.  The keys returned by this
  // iterator are internal keys encoded by AppendInternalKey in the
  // db/format.{h,cc} module.
  // 返回的时候，必须确保memtable任然存在
  Iterator* NewIterator();

  // Add an entry into memtable that maps key to value at the
  // specified sequence number and with the specified type.
  // Typically value will be empty if type==kTypeDeletion.
  // 增加key_value,这个sequence需要格外看一下
  void Add(SequenceNumber seq, ValueType type, const Slice& key,
           const Slice& value);

  // If memtable contains a value for key, store it in *value and return true.
  // If memtable contains a deletion for key, store a NotFound() error
  // in *status and return true.
  // Else, return false.
  // status表示是否查找的到
  // 如果有，类型是valueType返回value以及对应的tru是valueDeletion，那么返回对应的s的状态
  // 没有的话，status的状态变成了not found()
  bool Get(const LookupKey& key, std::string* value, Status* s);

 private:
  // 内部定义了两个iterator
  // 一个向后iterator
  // 一个向前iterator
  // 没用到呀
  // 友元函数，访问类中的public 和 private
  // 感觉仿佛拥有了一样
  friend class MemTableIterator;
  friend class MemTableBackwardIterator;

  // 内部定义比较器comparator
  // 很漂亮的接口形式
  struct KeyComparator {
    const InternalKeyComparator comparator;
    explicit KeyComparator(const InternalKeyComparator& c) : comparator(c) {}
    int operator()(const char* a, const char* b) const;
  };

  // 声明跳表结构是我的table
  typedef SkipList<const char*, KeyComparator> Table;

  ~MemTable();  // Private since only Unref() should be used to delete it

  KeyComparator comparator_;
  int refs_;
  // 分配内存
  Arena arena_;
  Table table_;
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_MEMTABLE_H_
