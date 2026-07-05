#include "memtable.h"

#include <cstddef>

namespace lsm {
namespace {

// Approximate per-entry overhead beyond the raw key and value bytes: the node
// object, its forward-pointer vector, and the two std::string control blocks.
// The accounting only needs to be good enough to trigger a flush near the
// configured threshold, so a fixed estimate is deliberate.
constexpr std::size_t kApproxNodeOverhead = 48;

}  // namespace

void MemTable::Put(std::string_view key, std::string_view value) {
  const SkipList::InsertResult r = table_.Insert(key, ValueTag::kValue, value);
  if (r.overwrote) {
    // key bytes and node overhead are unchanged; only the value size moves.
    memory_usage_ += value.size();
    memory_usage_ -= r.prev_value_bytes;
  } else {
    memory_usage_ += key.size() + value.size() + kApproxNodeOverhead;
  }
}

void MemTable::Delete(std::string_view key) {
  const SkipList::InsertResult r = table_.Insert(key, ValueTag::kTombstone, "");
  if (r.overwrote) {
    memory_usage_ -= r.prev_value_bytes;  // tombstone carries no value bytes
  } else {
    memory_usage_ += key.size() + kApproxNodeOverhead;
  }
}

MemTable::GetResult MemTable::Get(std::string_view key, std::string* value) const {
  const SkipList::Node* node = table_.Find(key);
  if (node == nullptr) {
    return GetResult::kNotPresent;
  }
  if (node->tag == ValueTag::kTombstone) {
    return GetResult::kDeleted;
  }
  *value = node->value;
  return GetResult::kFound;
}

}  // namespace lsm
