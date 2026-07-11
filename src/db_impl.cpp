#include "db_impl.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "compaction.h"
#include "file_util.h"
#include "manifest.h"
#include "sstable.h"
#include "wal.h"

namespace lsm {
namespace {

// The size tier of a table: how many factors of `ratio` its size is above the
// smallest tier. Tables in the same tier are "similarly sized" for compaction.
int SizeTier(std::uint64_t size, std::size_t ratio) {
  const double r = static_cast<double>(std::max<std::size_t>(ratio, 2));
  const double s = static_cast<double>(std::max<std::uint64_t>(size, 1));
  const int tier = static_cast<int>(std::log(s) / std::log(r));
  return std::max(tier, 0);
}

}  // namespace

DBImpl::DBImpl(Options options, std::string dir)
    : options_(options),
      dir_(std::move(dir)),
      mem_(std::make_unique<MemTable>()),
      tables_(std::make_shared<const TableSet>()) {}

DBImpl::~DBImpl() { StopBackground(); }

std::string DBImpl::WalPath() const { return (std::filesystem::path(dir_) / "wal.log").string(); }

std::string DBImpl::ManifestPath() const {
  return (std::filesystem::path(dir_) / "MANIFEST").string();
}

std::string DBImpl::TablePath(std::uint64_t number) const {
  return (std::filesystem::path(dir_) / (std::to_string(number) + ".sst")).string();
}

Status DBImpl::Recover() {
  ManifestState state;
  Status s = ReadManifest(ManifestPath(), &state);
  if (!s.ok()) {
    return s;
  }
  next_file_number_ = state.next_file_number;

  auto initial = std::make_shared<TableSet>();
  for (const std::uint64_t number : state.table_numbers) {
    std::shared_ptr<SSTableReader> reader;
    s = SSTableReader::Open(TablePath(number), &reader);
    if (!s.ok()) {
      return s;
    }
    const std::uint64_t size = reader->file_size();
    initial->push_back(TableHandle{number, size, std::move(reader)});
  }
  tables_ = std::move(initial);

  s = RecoverWal(WalPath(), [this](ValueTag tag, std::string_view key, std::string_view value) {
    if (tag == ValueTag::kValue) {
      mem_->Put(key, value);
    } else {
      mem_->Delete(key);
    }
  });
  if (!s.ok()) {
    return s;
  }

  s = WalWriter::Open(WalPath(), &wal_);
  if (!s.ok()) {
    return s;
  }

  bg_thread_ = std::thread(&DBImpl::BackgroundLoop, this);
  return Status::Ok();
}

Status DBImpl::Put(std::string_view key, std::string_view value) {
  if (key.empty()) {
    return Status::InvalidArgument("empty key");
  }
  const std::lock_guard<std::mutex> lock(mu_);
  Status s = wal_->Append(ValueTag::kValue, key, value);
  if (!s.ok()) {
    return s;
  }
  mem_->Put(key, value);
  if (mem_->ApproximateMemoryUsage() >= options_.memtable_flush_threshold_bytes) {
    return FlushMemTableLocked();
  }
  return Status::Ok();
}

Status DBImpl::Delete(std::string_view key) {
  if (key.empty()) {
    return Status::InvalidArgument("empty key");
  }
  const std::lock_guard<std::mutex> lock(mu_);
  Status s = wal_->Append(ValueTag::kTombstone, key, "");
  if (!s.ok()) {
    return s;
  }
  mem_->Delete(key);
  if (mem_->ApproximateMemoryUsage() >= options_.memtable_flush_threshold_bytes) {
    return FlushMemTableLocked();
  }
  return Status::Ok();
}

Status DBImpl::Get(std::string_view key, std::string* value) {
  if (value == nullptr) {
    return Status::InvalidArgument("null value out-pointer");
  }

  // Hold the lock only long enough to read the memtable and grab an immutable
  // snapshot of the table set; the sstable reads below run lock-free.
  std::shared_ptr<const TableSet> snapshot;
  {
    const std::lock_guard<std::mutex> lock(mu_);
    const MemTable::GetResult mem_result = mem_->Get(key, value);
    if (mem_result == MemTable::GetResult::kFound) {
      return Status::Ok();
    }
    if (mem_result == MemTable::GetResult::kDeleted) {
      return Status::NotFound();
    }
    snapshot = tables_;
  }

  for (const TableHandle& table : *snapshot) {
    SSTableReader::GetResult table_result = SSTableReader::GetResult::kNotPresent;
    Status s = table.reader->Get(key, value, &table_result);
    if (!s.ok()) {
      return s;
    }
    if (table_result == SSTableReader::GetResult::kFound) {
      return Status::Ok();
    }
    if (table_result == SSTableReader::GetResult::kDeleted) {
      return Status::NotFound();
    }
  }
  return Status::NotFound();
}

Status DBImpl::Close() {
  StopBackground();
  const std::lock_guard<std::mutex> lock(mu_);
  if (wal_ != nullptr) {
    return wal_->Close();
  }
  return Status::Ok();
}

Status DBImpl::CommitManifestLocked(const TableSet& set) {
  ManifestState state;
  state.next_file_number = next_file_number_;
  for (const TableHandle& table : set) {
    state.table_numbers.push_back(table.number);
  }
  return WriteManifest(ManifestPath(), state);
}

Status DBImpl::FlushMemTableLocked() {
  if (mem_->Empty()) {
    return Status::Ok();
  }

  const std::uint64_t number = next_file_number_++;
  constexpr std::size_t kFlushBlockSize = 4096;
  SSTableBuilder builder(kFlushBlockSize, options_.bloom_fpr, options_.enable_bloom_filters);
  auto it = mem_->NewIterator();
  for (it.SeekToFirst(); it.Valid(); it.Next()) {
    builder.Add(it.key(), it.tag(), it.value());
  }
  const std::string table_path = TablePath(number);
  const std::string contents = builder.Finish();
  Status s = WriteFileSync(table_path, contents);
  if (!s.ok()) {
    return s;
  }
  std::shared_ptr<SSTableReader> reader;
  s = SSTableReader::Open(table_path, &reader);
  if (!s.ok()) {
    return s;
  }

  auto new_tables = std::make_shared<TableSet>(*tables_);
  new_tables->insert(new_tables->begin(), TableHandle{number, contents.size(), std::move(reader)});

  // manifest commit is the durability point; a crash before it recovers from
  // the still-present wal and ignores the orphan sstable.
  s = CommitManifestLocked(*new_tables);
  if (!s.ok()) {
    return s;
  }
  tables_ = std::move(new_tables);

  // rotate the wal now that its records are durable in the sstable.
  s = wal_->Close();
  if (!s.ok()) {
    return s;
  }
  std::error_code ec;
  std::filesystem::remove(WalPath(), ec);
  s = WalWriter::Open(WalPath(), &wal_);
  if (!s.ok()) {
    return s;
  }
  mem_ = std::make_unique<MemTable>();

  bg_cv_.notify_one();  // a new table may have created a compactable tier
  return Status::Ok();
}

bool DBImpl::CompactionNeededLocked() const {
  const TableSet& set = *tables_;
  const std::size_t min_merge = std::max<std::size_t>(options_.compaction_min_merge, 2);
  std::size_t i = 0;
  while (i < set.size()) {
    const int tier = SizeTier(set[i].size, min_merge);
    std::size_t j = i;
    while (j < set.size() && SizeTier(set[j].size, min_merge) == tier) {
      ++j;
    }
    if (j - i >= min_merge) {
      return true;
    }
    i = j;
  }
  return false;
}

DBImpl::Compaction DBImpl::PickCompactionLocked() {
  Compaction c;
  const TableSet& set = *tables_;
  const std::size_t min_merge = std::max<std::size_t>(options_.compaction_min_merge, 2);

  std::size_t i = 0;
  while (i < set.size()) {
    const int tier = SizeTier(set[i].size, min_merge);
    std::size_t j = i;
    while (j < set.size() && SizeTier(set[j].size, min_merge) == tier) {
      ++j;
    }
    if (j - i >= min_merge) {
      for (std::size_t k = i; k < j; ++k) {
        c.input_numbers.push_back(set[k].number);
        c.input_readers.push_back(set[k].reader);
      }
      c.drop_tombstones = (j == set.size());  // the run reaches the oldest table
      c.output_number = next_file_number_++;
      c.output_path = TablePath(c.output_number);
      return c;
    }
    i = j;
  }
  return c;  // empty: nothing to do
}

Status DBImpl::RunCompaction(Compaction* c) const {
  Status s = CompactTables(c->input_readers, c->output_path, options_, c->drop_tombstones);
  if (!s.ok()) {
    return s;
  }
  s = SSTableReader::Open(c->output_path, &c->output_reader);
  if (!s.ok()) {
    return s;
  }
  c->output_size = c->output_reader->file_size();
  return Status::Ok();
}

void DBImpl::InstallCompactionLocked(const Compaction& c) {
  auto new_tables = std::make_shared<TableSet>();
  bool inserted = false;
  for (const TableHandle& table : *tables_) {
    const bool is_input = std::find(c.input_numbers.begin(), c.input_numbers.end(), table.number) !=
                          c.input_numbers.end();
    if (is_input) {
      if (!inserted) {
        new_tables->push_back(TableHandle{c.output_number, c.output_size, c.output_reader});
        inserted = true;
      }
      continue;  // drop the merged input
    }
    new_tables->push_back(table);
  }
  if (!inserted) {
    new_tables->push_back(TableHandle{c.output_number, c.output_size, c.output_reader});
  }

  const Status s = CommitManifestLocked(*new_tables);
  if (!s.ok()) {
    return;  // keep the old set; the output sstable is orphaned and ignored later
  }
  tables_ = std::move(new_tables);

  // deleting an input file is safe while a reader still holds it open: posix
  // keeps the inode until the last descriptor closes, and old snapshots keep
  // those readers alive.
  for (const std::uint64_t number : c.input_numbers) {
    std::error_code ec;
    std::filesystem::remove(TablePath(number), ec);
  }
}

void DBImpl::BackgroundLoop() {
  std::unique_lock<std::mutex> lock(mu_);
  while (true) {
    bg_cv_.wait(lock, [this] { return bg_shutdown_ || CompactionNeededLocked(); });
    if (bg_shutdown_) {
      return;
    }
    Compaction c = PickCompactionLocked();
    if (c.input_numbers.empty()) {
      continue;
    }

    bg_running_ = true;
    lock.unlock();
    const Status s = RunCompaction(&c);  // heavy merge, no lock held
    lock.lock();
    if (s.ok()) {
      InstallCompactionLocked(c);
    }
    bg_running_ = false;
    idle_cv_.notify_all();
  }
}

void DBImpl::StopBackground() {
  {
    const std::lock_guard<std::mutex> lock(mu_);
    bg_shutdown_ = true;
  }
  bg_cv_.notify_all();
  if (bg_thread_.joinable()) {
    bg_thread_.join();
  }
}

void DBImpl::TEST_WaitForCompaction() {
  std::unique_lock<std::mutex> lock(mu_);
  idle_cv_.wait(lock, [this] { return !bg_running_ && !CompactionNeededLocked(); });
}

std::size_t DBImpl::TEST_TableCount() {
  const std::lock_guard<std::mutex> lock(mu_);
  return tables_->size();
}

}  // namespace lsm
