#pragma once

#include <memory>
#include <string>
#include <vector>

#include "lsm/db.h"
#include "sstable.h"

namespace lsm {

// Merges a set of sstables (ordered newest-first) into a single new sstable
// written durably at out_path. For each key only the newest version survives,
// so shadowed values are dropped. A tombstone is dropped too when
// drop_tombstones is set, which is safe only when the inputs include the oldest
// table in the database (nothing older remains for the tombstone to shadow).
Status CompactTables(const std::vector<std::shared_ptr<SSTableReader>>& inputs_newest_first,
                     const std::string& out_path, const Options& options, bool drop_tombstones);

}  // namespace lsm
