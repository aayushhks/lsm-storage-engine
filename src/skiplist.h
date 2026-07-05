#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace lsm {

// Distinguishes a live value from a deletion marker (tombstone). Tombstones are
// kept, not erased: once flushed to an sstable a tombstone must shadow older
// values for the same key in older tables. A tombstone carries no value bytes.
enum class ValueTag : std::uint8_t { kValue, kTombstone };

// A skip list specialized as the lsm memtable: an ordered map from byte-string
// key to (tag, value), sorted lexicographically by key. Probabilistic balancing
// (no rotations) keeps it simple to implement and friendly to a future
// single-writer / multi-reader scheme (see docs/design.md). Not thread-safe on
// its own; callers serialize access.
class SkipList {
 public:
  struct Node {
    Node(std::string k, ValueTag t, std::string v, int height)
        : key(std::move(k)), tag(t), value(std::move(v)), next(static_cast<std::size_t>(height)) {}

    std::string key;
    ValueTag tag;
    std::string value;
    std::vector<Node*> next;  // forward pointers, one per level of this node
  };

  struct InsertResult {
    bool overwrote;                // an existing key was replaced
    std::size_t prev_value_bytes;  // value bytes of the replaced entry (0 if new)
  };

  SkipList();
  ~SkipList() = default;
  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;
  SkipList(SkipList&&) = delete;
  SkipList& operator=(SkipList&&) = delete;

  // Inserts key with (tag, value), overwriting any existing entry for key.
  InsertResult Insert(std::string_view key, ValueTag tag, std::string_view value);

  // Returns the node for key, or nullptr if absent.
  const Node* Find(std::string_view key) const;

  std::size_t Size() const { return num_entries_; }

  // Forward, ordered iterator. Yields every entry including tombstones, which is
  // what the sstable flush (m4) needs.
  class Iterator {
   public:
    explicit Iterator(const SkipList* list) : list_(list) {}

    bool Valid() const { return node_ != nullptr; }
    void SeekToFirst() { node_ = list_->head_->next[0]; }
    void Seek(std::string_view target);
    void Next();

    const std::string& key() const { return node_->key; }
    const std::string& value() const { return node_->value; }
    ValueTag tag() const { return node_->tag; }

   private:
    const SkipList* list_;
    const Node* node_ = nullptr;
  };

  Iterator NewIterator() const { return Iterator(this); }

 private:
  int RandomHeight();
  const Node* FindGreaterOrEqual(std::string_view key) const;

  std::unique_ptr<Node> head_;                // sentinel; keys start at next[]
  std::vector<std::unique_ptr<Node>> nodes_;  // owns every real node (raii)
  int height_ = 1;                            // current max level in use, >= 1
  std::size_t num_entries_ = 0;
  std::mt19937 rng_;
};

}  // namespace lsm
