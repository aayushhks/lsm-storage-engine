#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "skiplist.h"

namespace lsm {

// The mutable in-memory write buffer. Writes land here (after the wal, from m3);
// once it grows past the flush threshold it becomes an immutable sstable (m4).
// Wraps the skip list with byte accounting and tombstone-aware lookup. Not
// thread-safe; callers serialize access.
class MemTable {
 public:
  MemTable() = default;

  void Put(std::string_view key, std::string_view value);
  void Delete(std::string_view key);

  // kFound: live value written to *value. kDeleted: a tombstone shadows the key.
  // kNotPresent: the key is absent from this memtable. The kDeleted vs
  // kNotPresent distinction matters once older sstables can be consulted.
  enum class GetResult : std::uint8_t { kFound, kDeleted, kNotPresent };
  GetResult Get(std::string_view key, std::string* value) const;

  std::size_t ApproximateMemoryUsage() const { return memory_usage_; }
  std::size_t NumEntries() const { return table_.Size(); }
  bool Empty() const { return table_.Size() == 0; }

  using Iterator = SkipList::Iterator;
  Iterator NewIterator() const { return table_.NewIterator(); }

 private:
  SkipList table_;
  std::size_t memory_usage_ = 0;
};

}  // namespace lsm
