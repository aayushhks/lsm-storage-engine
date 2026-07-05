#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

#include "lsm/db.h"

namespace lsm {

// The concrete engine. In m1 the store is a placeholder in-memory map guarded
// by a mutex, which is enough to exercise the public contract, the test rig,
// the sanitizers, and ci. m2 replaces it with the hand-written skip-list
// memtable behind this same interface; m3 adds the wal; m4 adds sstable flush;
// later milestones add bloom filters and background compaction.
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

  // Placeholder store (m1 only). Tombstones are not modeled yet: Delete simply
  // erases. Real tombstone semantics arrive with the memtable in m2. The
  // transparent comparator lets us look up by string_view without allocating.
  std::map<std::string, std::string, std::less<>> store_;
};

}  // namespace lsm
