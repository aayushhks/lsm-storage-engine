#pragma once

#include <mutex>
#include <string>
#include <string_view>

#include "lsm/db.h"
#include "memtable.h"

namespace lsm {

// The concrete engine. In m2 writes go to the skip-list memtable guarded by a
// mutex; there is no durability yet. m3 adds the wal in front of the memtable,
// m4 adds sstable flush, and later milestones add bloom filters and background
// compaction.
class DBImpl : public DB {
 public:
  explicit DBImpl(std::string dir);

  Status Put(std::string_view key, std::string_view value) override;
  Status Delete(std::string_view key) override;
  Status Get(std::string_view key, std::string* value) override;
  Status Close() override;

 private:
  std::string dir_;
  std::mutex mu_;
  MemTable mem_;
};

}  // namespace lsm
