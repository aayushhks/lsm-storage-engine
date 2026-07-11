#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "bloom.h"
#include "lsm/db.h"
#include "skiplist.h"  // ValueTag

namespace lsm {

// Process-wide count of sstable data blocks read from disk, a proxy for read
// amplification. Tests and benchmarks use it to show the effect of the bloom
// filters. Not reset automatically.
std::uint64_t SSTableDataBlockReads();
void ResetSSTableDataBlockReads();

// Builds an immutable sorted table in memory. Keys must be added in ascending
// order (the memtable iterator provides exactly that). Layout:
//   [data block 0][data block 1]...[index block][bloom block][footer]
// A data block is a run of entries: [keylen u32][key][tag u8][vallen u32][value].
// The sparse index has one entry per block: [keylen u32][first key][offset u64][size u32].
// The bloom block is a serialized filter over every key. The fixed 40-byte footer
// is [bloom offset u64][bloom size u64][index offset u64][index size u64][magic u64].
class SSTableBuilder {
 public:
  explicit SSTableBuilder(std::size_t block_size = 4096, double bloom_fpr = 0.01,
                          bool enable_bloom = true);

  // Appends one entry. Callers add keys in ascending order.
  void Add(std::string_view key, ValueTag tag, std::string_view value);

  // Finalizes and returns the complete table bytes.
  std::string Finish();

  std::size_t NumEntries() const { return num_entries_; }

 private:
  void FlushBlock();

  std::size_t block_size_;
  double bloom_fpr_;
  bool enable_bloom_;
  std::string file_;                         // assembled table bytes
  std::string block_;                        // current in-progress data block
  std::string block_first_;                  // first key of the current block
  std::string index_;                        // assembled index-entry bytes
  std::vector<std::uint64_t> bloom_hashes_;  // one hash per key, for the filter
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

  // Ordered scan support, used by compaction. Reads are positioned (pread), so
  // these are safe to call from a background thread while foreground lookups run.
  std::size_t NumDataBlocks() const { return index_.size(); }
  Status ReadDataBlock(std::size_t block, std::string* out) const;
  std::uint64_t file_size() const { return file_size_; }

 private:
  struct IndexEntry {
    std::string first_key;
    std::uint64_t offset;
    std::uint32_t size;
  };

  Status LoadIndex();

  int fd_;
  std::uint64_t file_size_ = 0;
  std::vector<IndexEntry> index_;
  BloomFilter bloom_;
};

// Forward, ordered iterator over every entry in a table (values and tombstones
// alike), used by the k-way merge in compaction. Holds the reader alive.
class SSTableIterator {
 public:
  explicit SSTableIterator(std::shared_ptr<const SSTableReader> reader);

  bool Valid() const { return valid_; }
  const Status& status() const { return status_; }
  void SeekToFirst();
  void Next();

  // Valid until the next call to Next(); callers copy what they keep.
  std::string_view key() const { return key_; }
  ValueTag tag() const { return tag_; }
  std::string_view value() const { return value_; }

 private:
  void Advance();

  std::shared_ptr<const SSTableReader> reader_;
  std::size_t block_index_ = 0;
  std::string block_;
  std::string_view cursor_;
  std::string_view key_;
  std::string_view value_;
  ValueTag tag_ = ValueTag::kValue;
  bool valid_ = false;
  Status status_;
};

}  // namespace lsm
