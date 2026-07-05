#include "skiplist.h"

#include <array>
#include <cstddef>
#include <string>
#include <utility>

namespace lsm {
namespace {

// Level generation. Starting at level 1, a node gains another level with
// probability 1/kBranching, capped at kMaxHeight. kBranching = 4 keeps the
// index sparse (about 1.33 forward pointers per node on average) while still
// giving O(log n) search; kMaxHeight = 12 supports ~4^12 entries, far past what
// a memtable holds before flushing.
constexpr int kMaxHeight = 12;
constexpr int kBranching = 4;

}  // namespace

SkipList::SkipList()
    : head_(std::make_unique<Node>(std::string{}, ValueTag::kValue, std::string{}, kMaxHeight)),
      rng_(0xC0FFEE) {}

int SkipList::RandomHeight() {
  int height = 1;
  while (height < kMaxHeight && (rng_() % kBranching) == 0) {
    ++height;
  }
  return height;
}

const SkipList::Node* SkipList::FindGreaterOrEqual(std::string_view key) const {
  const Node* x = head_.get();
  for (int level = height_ - 1; level >= 0; --level) {
    const Node* next = x->next[level];
    while (next != nullptr && next->key < key) {
      x = next;
      next = x->next[level];
    }
  }
  return x->next[0];
}

const SkipList::Node* SkipList::Find(std::string_view key) const {
  const Node* node = FindGreaterOrEqual(key);
  if (node != nullptr && node->key == key) {
    return node;
  }
  return nullptr;
}

SkipList::InsertResult SkipList::Insert(std::string_view key, ValueTag tag,
                                        std::string_view value) {
  // prev[level] is the last node at that level whose key is < the search key,
  // i.e. the node whose forward pointer we may need to splice. Every index below
  // is a level in [0, kMaxHeight), bounded by construction, so the fixed-array
  // bounds check is suppressed here rather than paying for runtime .at() on the
  // hot write path.
  std::array<Node*, kMaxHeight> prev{};
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index)
  Node* x = head_.get();
  for (int level = height_ - 1; level >= 0; --level) {
    Node* next = x->next[level];
    while (next != nullptr && next->key < key) {
      x = next;
      next = x->next[level];
    }
    prev[level] = x;
  }

  Node* candidate = x->next[0];
  if (candidate != nullptr && candidate->key == key) {
    const std::size_t prev_value_bytes = candidate->value.size();
    candidate->tag = tag;
    candidate->value.assign(value);
    return InsertResult{true, prev_value_bytes};
  }

  const int height = RandomHeight();
  if (height > height_) {
    for (int level = height_; level < height; ++level) {
      prev[level] = head_.get();
    }
    height_ = height;
  }

  auto owned = std::make_unique<Node>(std::string(key), tag, std::string(value), height);
  Node* node = owned.get();
  for (int level = 0; level < height; ++level) {
    node->next[level] = prev[level]->next[level];
    prev[level]->next[level] = node;
  }
  // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)
  nodes_.push_back(std::move(owned));
  ++num_entries_;
  return InsertResult{false, 0};
}

void SkipList::Iterator::Next() { node_ = node_->next[0]; }

void SkipList::Iterator::Seek(std::string_view target) {
  node_ = list_->FindGreaterOrEqual(target);
}

}  // namespace lsm
