#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace lsm {

// Exception-free error signaling. Every fallible operation returns a Status.
// Reads and writes gain real i/o and corruption failure modes once the wal and
// sstables land, so a single status type across the whole surface is cleaner
// than mixing an optional return with a separate error channel.
class Status {
 public:
  enum class Code : std::uint8_t {
    kOk,
    kNotFound,
    kCorruption,
    kIOError,
    kInvalidArgument,
  };

  Status();  // ok

  static Status Ok();
  static Status NotFound(std::string msg = {});
  static Status Corruption(std::string msg);
  static Status IOError(std::string msg);
  static Status InvalidArgument(std::string msg);

  bool ok() const;
  bool IsNotFound() const;
  bool IsCorruption() const;
  bool IsIOError() const;
  bool IsInvalidArgument() const;

  Code code() const;
  const std::string& message() const;
  std::string ToString() const;

 private:
  Status(Code code, std::string message);

  Code code_;
  std::string message_;
};

// Tunables. Grows milestone by milestone (compaction fan-in, ...).
struct Options {
  bool create_if_missing = true;

  // Memtable size at which a flush is triggered. Consumed from m4 onward.
  std::size_t memtable_flush_threshold_bytes = 4U << 20;  // 4 MiB

  // Target false-positive rate for the per-sstable bloom filters. Smaller means
  // fewer wasted block reads on a miss at the cost of a larger filter.
  double bloom_fpr = 0.01;

  // Build a bloom filter into each sstable. Disable to measure the read-path
  // cost without it.
  bool enable_bloom_filters = true;

  // Size-tiered compaction merges a run of similarly sized sstables once this
  // many accumulate in a tier. Also the size ratio between adjacent tiers.
  std::size_t compaction_min_merge = 4;
};

// Single-node key-value store. Keys and values are arbitrary byte strings
// (embedded nul bytes are fine). This abstract interface is the only header a
// user includes; the concrete implementation lives in src/.
class DB {
 public:
  // Opens the database rooted at `dir`, creating it when Options::create_if_missing
  // is set. On success `*handle` owns the open database. A polymorphic base held
  // through a unique_ptr, so it is neither copyable nor movable.
  static Status Open(const Options& options, const std::string& dir, std::unique_ptr<DB>* handle);

  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;
  DB(DB&&) = delete;
  DB& operator=(DB&&) = delete;
  virtual ~DB() = default;

  virtual Status Put(std::string_view key, std::string_view value) = 0;
  virtual Status Delete(std::string_view key) = 0;

  // Looks up `key`. On a hit, writes the value to `*value` and returns ok.
  // A missing key returns Status::NotFound; other failures return an i/o or
  // corruption status.
  virtual Status Get(std::string_view key, std::string* value) = 0;

  // Flushes and releases resources. Also invoked by the destructor, so an
  // explicit call is optional. Idempotent.
  virtual Status Close() = 0;

 protected:
  DB() = default;
};

}  // namespace lsm
