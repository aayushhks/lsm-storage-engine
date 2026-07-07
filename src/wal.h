#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "lsm/db.h"
#include "skiplist.h"  // ValueTag

namespace lsm {

// Append-only write-ahead log. Each record is framed as:
//   [payload length : u32 le][crc32 of payload : u32 le][payload bytes]
// and the payload is one encoded mutation:
//   [type : u8][key length : u32 le][key][value length : u32 le][value]
// Every append is fsync'd before it returns, so a mutation is durable before
// the memtable is updated (see docs/design.md on fsync-per-write vs. group
// commit). Not thread-safe; callers serialize appends.
class WalWriter {
 public:
  // Opens (creating if needed) the log at path for appending. On success
  // *out owns the writer.
  static Status Open(const std::string& path, std::unique_ptr<WalWriter>* out);

  // Takes ownership of an already-open, append-mode file descriptor. Prefer
  // Open(); this is public only so the factory can use make_unique.
  WalWriter(int fd, std::string path);
  ~WalWriter();
  WalWriter(const WalWriter&) = delete;
  WalWriter& operator=(const WalWriter&) = delete;
  WalWriter(WalWriter&&) = delete;
  WalWriter& operator=(WalWriter&&) = delete;

  // Frames and appends the mutation, then fsyncs. Returns only after the bytes
  // are durable.
  Status Append(ValueTag tag, std::string_view key, std::string_view value);

  // Flushes and closes the underlying descriptor. Idempotent.
  Status Close();

 private:
  Status WriteAll(std::string_view bytes);

  int fd_;
  std::string path_;
};

// Invoked for each valid record during recovery, in append order.
using WalApplyFn = std::function<void(ValueTag tag, std::string_view key, std::string_view value)>;

// Replays the wal at path, calling apply for each valid record in order. A torn
// or crc-invalid tail record is detected and the file is truncated at the last
// good boundary, so recovery never fails on a bad tail. A missing file is not an
// error. Returns non-ok only on an actual i/o failure.
Status RecoverWal(const std::string& path, const WalApplyFn& apply);

}  // namespace lsm
