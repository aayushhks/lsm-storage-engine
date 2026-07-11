#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "lsm/db.h"
#include "memtable.h"
#include "sstable.h"
#include "wal.h"

namespace lsm {

// The concrete engine. In m4 writes go to the wal and the memtable; when the
// memtable crosses the flush threshold it is written out as an immutable
// sstable, the manifest is updated atomically, and the wal is rotated away.
// Reads consult the memtable first, then the sstables newest-to-oldest.
class DBImpl : public DB {
 public:
  DBImpl(Options options, std::string dir);

  // Loads the manifest and its sstables, replays the wal into the memtable, and
  // opens the wal for appending. Called by DB::Open before the handle escapes.
  Status Recover();

  Status Put(std::string_view key, std::string_view value) override;
  Status Delete(std::string_view key) override;
  Status Get(std::string_view key, std::string* value) override;
  Status Close() override;

 private:
  struct TableInfo {
    std::uint64_t number;
    std::shared_ptr<SSTableReader> reader;
  };

  std::string WalPath() const;
  std::string ManifestPath() const;
  std::string TablePath(std::uint64_t number) const;

  Status MaybeFlush();     // flush if over threshold; caller holds mu_
  Status FlushMemTable();  // write the memtable as an sstable; caller holds mu_

  Options options_;
  std::string dir_;
  std::mutex mu_;
  std::unique_ptr<MemTable> mem_;
  std::unique_ptr<WalWriter> wal_;
  std::vector<TableInfo> tables_;  // newest first
  std::uint64_t next_file_number_ = 1;
};

}  // namespace lsm
