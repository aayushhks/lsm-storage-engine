#include "db_impl.h"

#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include "wal.h"

namespace lsm {

DBImpl::DBImpl(std::string dir) : dir_(std::move(dir)) {}

std::string DBImpl::WalPath() const { return (std::filesystem::path(dir_) / "wal.log").string(); }

Status DBImpl::Recover() {
  const std::string wal_path = WalPath();
  Status s =
      RecoverWal(wal_path, [this](ValueTag tag, std::string_view key, std::string_view value) {
        if (tag == ValueTag::kValue) {
          mem_.Put(key, value);
        } else {
          mem_.Delete(key);
        }
      });
  if (!s.ok()) {
    return s;
  }
  return WalWriter::Open(wal_path, &wal_);
}

Status DBImpl::Put(std::string_view key, std::string_view value) {
  if (key.empty()) {
    return Status::InvalidArgument("empty key");
  }
  const std::lock_guard<std::mutex> lock(mu_);
  // durable in the wal before it is visible in the memtable.
  Status s = wal_->Append(ValueTag::kValue, key, value);
  if (!s.ok()) {
    return s;
  }
  mem_.Put(key, value);
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
  mem_.Delete(key);
  return Status::Ok();
}

Status DBImpl::Get(std::string_view key, std::string* value) {
  if (value == nullptr) {
    return Status::InvalidArgument("null value out-pointer");
  }
  const std::lock_guard<std::mutex> lock(mu_);
  // a tombstone and an absent key both read as not-found here; the distinction
  // only matters once older sstables can be consulted (m5).
  if (mem_.Get(key, value) == MemTable::GetResult::kFound) {
    return Status::Ok();
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

}  // namespace lsm
