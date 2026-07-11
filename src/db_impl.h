#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "lsm/db.h"
#include "memtable.h"
#include "sstable.h"
#include "wal.h"

namespace lsm {

// The concrete engine. In m6 a single background thread performs size-tiered
// compaction. Synchronization uses an immutable, reference-counted snapshot of
// the table set: foreground reads copy the snapshot pointer under a short-lived
// lock and then read sstables with no lock held, while compaction publishes a
// new snapshot atomically. A reader holding an old snapshot keeps the old
// readers (and their open file descriptors) alive, so a compaction that swaps
// and deletes files never breaks an in-flight read. See docs/design.md.
class DBImpl : public DB {
 public:
  DBImpl(Options options, std::string dir);
  ~DBImpl() override;

  DBImpl(const DBImpl&) = delete;
  DBImpl& operator=(const DBImpl&) = delete;
  DBImpl(DBImpl&&) = delete;
  DBImpl& operator=(DBImpl&&) = delete;

  // Loads the manifest and sstables, replays the wal, opens the wal for
  // appending, and starts the background compaction thread.
  Status Recover();

  Status Put(std::string_view key, std::string_view value) override;
  Status Delete(std::string_view key) override;
  Status Get(std::string_view key, std::string* value) override;
  Status Close() override;

  // Test hooks: block until the background thread is idle with no compaction
  // pending, and report the current number of live tables.
  void TEST_WaitForCompaction();
  std::size_t TEST_TableCount();

 private:
  struct TableHandle {
    std::uint64_t number;
    std::uint64_t size;
    std::shared_ptr<SSTableReader> reader;
  };
  using TableSet = std::vector<TableHandle>;

  struct Compaction {
    std::vector<std::uint64_t> input_numbers;                   // tables removed
    std::vector<std::shared_ptr<SSTableReader>> input_readers;  // newest-first, kept alive
    std::uint64_t output_number = 0;
    std::string output_path;
    bool drop_tombstones = false;
    std::shared_ptr<SSTableReader> output_reader;
    std::uint64_t output_size = 0;
  };

  std::string WalPath() const;
  std::string ManifestPath() const;
  std::string TablePath(std::uint64_t number) const;

  Status FlushMemTableLocked();  // build+publish an sstable; caller holds mu_

  void BackgroundLoop();
  bool CompactionNeededLocked() const;
  Compaction PickCompactionLocked();          // selects inputs, allocates output number
  Status RunCompaction(Compaction* c) const;  // heavy merge, no lock
  void InstallCompactionLocked(const Compaction& c);
  void StopBackground();
  Status CommitManifestLocked(const TableSet& set);

  Options options_;
  std::string dir_;

  mutable std::mutex mu_;
  std::unique_ptr<MemTable> mem_;
  std::unique_ptr<WalWriter> wal_;
  std::shared_ptr<const TableSet> tables_;  // immutable snapshot, newest first
  std::uint64_t next_file_number_ = 1;

  std::thread bg_thread_;
  std::condition_variable bg_cv_;
  std::condition_variable idle_cv_;
  bool bg_shutdown_ = false;
  bool bg_running_ = false;
};

}  // namespace lsm
