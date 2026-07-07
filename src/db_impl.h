#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "lsm/db.h"
#include "memtable.h"
#include "wal.h"

namespace lsm {

// The concrete engine. In m3 every mutation is appended and fsync'd to the wal
// before it touches the skip-list memtable, and Open replays the wal to rebuild
// the memtable. m4 adds sstable flush (which lets the wal be rotated away).
class DBImpl : public DB {
 public:
  explicit DBImpl(std::string dir);

  // Replays any existing wal into the memtable and opens the wal for appending.
  // Called by DB::Open before the handle is returned.
  Status Recover();

  Status Put(std::string_view key, std::string_view value) override;
  Status Delete(std::string_view key) override;
  Status Get(std::string_view key, std::string* value) override;
  Status Close() override;

 private:
  std::string WalPath() const;

  std::string dir_;
  std::mutex mu_;
  MemTable mem_;
  std::unique_ptr<WalWriter> wal_;
};

}  // namespace lsm
