#include "db_impl.h"

#include <mutex>
#include <string>
#include <utility>

namespace lsm {

DBImpl::DBImpl(std::string dir) : dir_(std::move(dir)) {}

Status DBImpl::Put(std::string_view key, std::string_view value) {
  if (key.empty()) {
    return Status::InvalidArgument("empty key");
  }
  const std::lock_guard<std::mutex> lock(mu_);
  mem_.Put(key, value);
  return Status::Ok();
}

Status DBImpl::Delete(std::string_view key) {
  if (key.empty()) {
    return Status::InvalidArgument("empty key");
  }
  const std::lock_guard<std::mutex> lock(mu_);
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
  // nothing durable to flush yet; the wal and sstable flush arrive in m3/m4.
  return Status::Ok();
}

}  // namespace lsm
