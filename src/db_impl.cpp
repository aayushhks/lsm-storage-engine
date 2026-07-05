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
  store_.insert_or_assign(std::string(key), std::string(value));
  return Status::Ok();
}

Status DBImpl::Delete(std::string_view key) {
  if (key.empty()) {
    return Status::InvalidArgument("empty key");
  }
  const std::lock_guard<std::mutex> lock(mu_);
  store_.erase(std::string(key));
  return Status::Ok();
}

Status DBImpl::Get(std::string_view key, std::string* value) {
  if (value == nullptr) {
    return Status::InvalidArgument("null value out-pointer");
  }
  const std::lock_guard<std::mutex> lock(mu_);
  const auto it = store_.find(key);
  if (it == store_.end()) {
    return Status::NotFound();
  }
  *value = it->second;
  return Status::Ok();
}

Status DBImpl::Close() {
  // Nothing durable to flush yet; the wal and sstable flush arrive in m3/m4.
  return Status::Ok();
}

}  // namespace lsm
