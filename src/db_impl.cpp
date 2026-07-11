#include "db_impl.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "file_util.h"
#include "manifest.h"
#include "sstable.h"
#include "wal.h"

namespace lsm {

DBImpl::DBImpl(Options options, std::string dir)
    : options_(options), dir_(std::move(dir)), mem_(std::make_unique<MemTable>()) {}

std::string DBImpl::WalPath() const { return (std::filesystem::path(dir_) / "wal.log").string(); }

std::string DBImpl::ManifestPath() const {
  return (std::filesystem::path(dir_) / "MANIFEST").string();
}

std::string DBImpl::TablePath(std::uint64_t number) const {
  return (std::filesystem::path(dir_) / (std::to_string(number) + ".sst")).string();
}

Status DBImpl::Recover() {
  // 1. load the manifest and open every live sstable, newest first.
  ManifestState state;
  Status s = ReadManifest(ManifestPath(), &state);
  if (!s.ok()) {
    return s;
  }
  next_file_number_ = state.next_file_number;
  for (const std::uint64_t number : state.table_numbers) {
    std::shared_ptr<SSTableReader> reader;
    s = SSTableReader::Open(TablePath(number), &reader);
    if (!s.ok()) {
      return s;
    }
    tables_.push_back(TableInfo{number, std::move(reader)});
  }

  // 2. replay the wal (mutations since the last flush) into the memtable.
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

  // 3. open the wal for appending.
  return WalWriter::Open(WalPath(), &wal_);
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
  return MaybeFlush();
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
  return MaybeFlush();
}

Status DBImpl::Get(std::string_view key, std::string* value) {
  if (value == nullptr) {
    return Status::InvalidArgument("null value out-pointer");
  }
  const std::lock_guard<std::mutex> lock(mu_);

  // the memtable holds the newest writes; a hit or tombstone here wins outright.
  const MemTable::GetResult mem_result = mem_->Get(key, value);
  if (mem_result == MemTable::GetResult::kFound) {
    return Status::Ok();
  }
  if (mem_result == MemTable::GetResult::kDeleted) {
    return Status::NotFound();
  }

  // then the sstables, newest to oldest; the first hit or tombstone wins.
  for (const TableInfo& table : tables_) {
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
  const std::lock_guard<std::mutex> lock(mu_);
  if (wal_ != nullptr) {
    return wal_->Close();
  }
  return Status::Ok();
}

Status DBImpl::MaybeFlush() {
  if (mem_->ApproximateMemoryUsage() >= options_.memtable_flush_threshold_bytes) {
    return FlushMemTable();
  }
  return Status::Ok();
}

Status DBImpl::FlushMemTable() {
  if (mem_->Empty()) {
    return Status::Ok();
  }

  // 1. build the sstable from the sorted memtable and write it durably.
  const std::uint64_t number = next_file_number_++;
  constexpr std::size_t kFlushBlockSize = 4096;
  SSTableBuilder builder(kFlushBlockSize, options_.bloom_fpr, options_.enable_bloom_filters);
  auto it = mem_->NewIterator();
  for (it.SeekToFirst(); it.Valid(); it.Next()) {
    builder.Add(it.key(), it.tag(), it.value());
  }
  const std::string table_path = TablePath(number);
  Status s = WriteFileSync(table_path, builder.Finish());
  if (!s.ok()) {
    return s;
  }
  std::shared_ptr<SSTableReader> reader;
  s = SSTableReader::Open(table_path, &reader);
  if (!s.ok()) {
    return s;
  }

  // 2. commit the manifest atomically. this is the point at which the flush
  // becomes durable: a crash before here recovers from the still-intact wal and
  // ignores the orphan sstable; a crash after here finds the table in the
  // manifest. only after the commit is the wal safe to discard.
  ManifestState state;
  state.next_file_number = next_file_number_;
  state.table_numbers.push_back(number);
  for (const TableInfo& table : tables_) {
    state.table_numbers.push_back(table.number);
  }
  s = WriteManifest(ManifestPath(), state);
  if (!s.ok()) {
    return s;  // sstable is orphaned on disk and ignored on the next open.
  }
  tables_.insert(tables_.begin(), TableInfo{number, std::move(reader)});

  // 3. rotate the wal: its records are now durable in the sstable.
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

  // 4. reset the memtable.
  mem_ = std::make_unique<MemTable>();
  return Status::Ok();
}

}  // namespace lsm
