#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "lsm/db.h"
#include "skiplist.h"  // ValueTag

namespace lsm {

// Builds an immutable sorted table in memory. Keys must be added in ascending
// order (the memtable iterator provides exactly that). Layout:
//   [data block 0][data block 1]...[index block][footer]
// A data block is a run of entries: [keylen u32][key][tag u8][vallen u32][value].
// The sparse index has one entry per block: [keylen u32][first key][offset u64][size u32].
// The fixed 24-byte footer is [index offset u64][index size u64][magic u64].
class SSTableBuilder {
 public:
  explicit SSTableBuilder(std::size_t block_size = 4096);

  // Appends one entry. Callers add keys in ascending order.
  void Add(std::string_view key, ValueTag tag, std::string_view value);

  // Finalizes and returns the complete table bytes.
  std::string Finish();

  std::size_t NumEntries() const { return num_entries_; }

 private:
  void FlushBlock();

  std::size_t block_size_;
  std::string file_;         // assembled table bytes
  std::string block_;        // current in-progress data block
  std::string block_first_;  // first key of the current block
  std::string index_;        // assembled index-entry bytes
  std::size_t num_entries_ = 0;
};

// Reads a table produced by SSTableBuilder. Loads the sparse index into memory
// on open and preads data blocks on demand; pread is positioned and stateless,
// which keeps the reader safe to share across threads later (m6).
class SSTableReader {
 public:
  enum class GetResult : std::uint8_t { kFound, kDeleted, kNotPresent };

  static Status Open(const std::string& path, std::shared_ptr<SSTableReader>* out);

  explicit SSTableReader(int fd);
  ~SSTableReader();
  SSTableReader(const SSTableReader&) = delete;
  SSTableReader& operator=(const SSTableReader&) = delete;
  SSTableReader(SSTableReader&&) = delete;
  SSTableReader& operator=(SSTableReader&&) = delete;

  // Looks up key. Returns non-ok only on an i/o or corruption error; a normal
  // hit/miss/tombstone is reported through *result (and *value on a hit).
  Status Get(std::string_view key, std::string* value, GetResult* result) const;

 private:
  struct IndexEntry {
    std::string first_key;
    std::uint64_t offset;
    std::uint32_t size;
  };

  Status LoadIndex();

  int fd_;
  std::vector<IndexEntry> index_;
};

}  // namespace lsm
