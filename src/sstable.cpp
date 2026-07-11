#include "sstable.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>
#include <system_error>
#include <utility>

#include "coding.h"

namespace lsm {
namespace {

// Identifies the table format (and its version). The reader rejects any file
// whose footer does not end with this value. Bumped from the m4 layout because
// the footer now also carries the bloom block location.
constexpr std::uint64_t kMagic = 0x6C736D5F53535432ULL;  // "lsm_SST2"
constexpr std::size_t kFooterSize = 40;  // bloom off/size + index off/size + magic, all u64

std::atomic<std::uint64_t>& DataBlockReadCounter() {
  static std::atomic<std::uint64_t> counter{0};
  return counter;
}

Status PosixError(const std::string& context) {
  const std::error_code ec(errno, std::generic_category());
  return Status::IOError(context + ": " + ec.message());
}

// Reads exactly length bytes from fd at offset into the first length bytes of
// *out. Precondition: out->size() >= length. Unlike a resize-then-read, this
// does not zero-fill the buffer first (pread overwrites it anyway), which is the
// dominant cost on the read path when the buffer is reused.
Status PreadInto(int fd, std::uint64_t offset, std::size_t length, std::string* out) {
  std::size_t done = 0;
  while (done < length) {
    const ssize_t n = ::pread(fd, &(*out)[done], length - done, static_cast<off_t>(offset + done));
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return PosixError("pread sstable");
    }
    if (n == 0) {
      return Status::Corruption("unexpected eof in sstable");
    }
    done += static_cast<std::size_t>(n);
  }
  return Status::Ok();
}

// Convenience for callers that want a freshly sized buffer (compaction scans).
Status PreadExact(int fd, std::uint64_t offset, std::size_t length, std::string* out) {
  out->resize(length);
  return PreadInto(fd, offset, length, out);
}

// Parses one data-block entry from *cursor, advancing it. Returns false if the
// bytes are structurally invalid.
bool ParseEntry(std::string_view* cursor, std::string_view* key, ValueTag* tag,
                std::string_view* value) {
  std::string_view c = *cursor;
  if (c.size() < sizeof(std::uint32_t)) {
    return false;
  }
  const std::uint32_t key_len = DecodeFixed32(c);
  c.remove_prefix(sizeof(std::uint32_t));
  if (c.size() < key_len + 1) {
    return false;
  }
  *key = c.substr(0, key_len);
  c.remove_prefix(key_len);

  const auto type = static_cast<unsigned char>(c[0]);
  c.remove_prefix(1);
  if (c.size() < sizeof(std::uint32_t)) {
    return false;
  }
  const std::uint32_t value_len = DecodeFixed32(c);
  c.remove_prefix(sizeof(std::uint32_t));
  if (c.size() < value_len) {
    return false;
  }
  *value = c.substr(0, value_len);
  c.remove_prefix(value_len);

  if (type == static_cast<unsigned char>(ValueTag::kValue)) {
    *tag = ValueTag::kValue;
  } else if (type == static_cast<unsigned char>(ValueTag::kTombstone)) {
    *tag = ValueTag::kTombstone;
  } else {
    return false;
  }
  *cursor = c;
  return true;
}

}  // namespace

std::uint64_t SSTableDataBlockReads() { return DataBlockReadCounter().load(); }
void ResetSSTableDataBlockReads() { DataBlockReadCounter().store(0); }

SSTableBuilder::SSTableBuilder(std::size_t block_size, double bloom_fpr, bool enable_bloom)
    : block_size_(block_size), bloom_fpr_(bloom_fpr), enable_bloom_(enable_bloom) {}

void SSTableBuilder::Add(std::string_view key, ValueTag tag, std::string_view value) {
  if (block_.empty()) {
    block_first_.assign(key);
  }
  PutFixed32(&block_, static_cast<std::uint32_t>(key.size()));
  block_.append(key);
  block_.push_back(static_cast<char>(static_cast<std::uint8_t>(tag)));
  PutFixed32(&block_, static_cast<std::uint32_t>(value.size()));
  block_.append(value);
  if (enable_bloom_) {
    bloom_hashes_.push_back(BloomHash(key));
  }
  ++num_entries_;
  if (block_.size() >= block_size_) {
    FlushBlock();
  }
}

void SSTableBuilder::FlushBlock() {
  if (block_.empty()) {
    return;
  }
  const std::uint64_t offset = file_.size();
  const auto size = static_cast<std::uint32_t>(block_.size());
  PutFixed32(&index_, static_cast<std::uint32_t>(block_first_.size()));
  index_.append(block_first_);
  PutFixed64(&index_, offset);
  PutFixed32(&index_, size);
  file_.append(block_);
  block_.clear();
  block_first_.clear();
}

std::string SSTableBuilder::Finish() {
  FlushBlock();  // flush the trailing block
  const std::uint64_t index_offset = file_.size();
  file_.append(index_);
  const std::uint64_t index_size = file_.size() - index_offset;

  const std::uint64_t bloom_offset = file_.size();
  if (enable_bloom_) {
    file_.append(BuildBloomFilter(bloom_hashes_, bloom_fpr_));
  }
  const std::uint64_t bloom_size = file_.size() - bloom_offset;

  PutFixed64(&file_, bloom_offset);
  PutFixed64(&file_, bloom_size);
  PutFixed64(&file_, index_offset);
  PutFixed64(&file_, index_size);
  PutFixed64(&file_, kMagic);
  return std::move(file_);
}

Status SSTableReader::Open(const std::string& path, std::shared_ptr<SSTableReader>* out) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) posix open is variadic by declaration
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return PosixError("open sstable " + path);
  }
  auto reader = std::make_shared<SSTableReader>(fd);
  Status s = reader->LoadIndex();
  if (!s.ok()) {
    return s;  // reader's destructor closes fd
  }
  *out = std::move(reader);
  return Status::Ok();
}

SSTableReader::SSTableReader(int fd) : fd_(fd) {}

SSTableReader::~SSTableReader() {
  if (fd_ >= 0) {
    static_cast<void>(::close(fd_));
    fd_ = -1;
  }
}

Status SSTableReader::LoadIndex() {
  struct stat st {};
  if (::fstat(fd_, &st) != 0) {
    return PosixError("fstat sstable");
  }
  const auto file_size = static_cast<std::uint64_t>(st.st_size);
  if (file_size < kFooterSize) {
    return Status::Corruption("sstable smaller than footer");
  }
  file_size_ = file_size;

  std::string footer;
  Status s = PreadExact(fd_, file_size - kFooterSize, kFooterSize, &footer);
  if (!s.ok()) {
    return s;
  }
  const std::string_view fv(footer);
  const std::uint64_t bloom_offset = DecodeFixed64(fv);
  const std::uint64_t bloom_size = DecodeFixed64(fv.substr(8));
  const std::uint64_t index_offset = DecodeFixed64(fv.substr(16));
  const std::uint64_t index_size = DecodeFixed64(fv.substr(24));
  const std::uint64_t magic = DecodeFixed64(fv.substr(32));
  if (magic != kMagic) {
    return Status::Corruption("sstable bad magic");
  }
  if (index_offset + index_size + kFooterSize > file_size ||
      bloom_offset + bloom_size + kFooterSize > file_size) {
    return Status::Corruption("sstable footer offsets out of range");
  }

  if (bloom_size > 0) {
    std::string bloom_bytes;
    s = PreadExact(fd_, bloom_offset, bloom_size, &bloom_bytes);
    if (!s.ok()) {
      return s;
    }
    bloom_ = BloomFilter(std::move(bloom_bytes));
  }

  std::string index_bytes;
  s = PreadExact(fd_, index_offset, index_size, &index_bytes);
  if (!s.ok()) {
    return s;
  }

  std::string_view iv(index_bytes);
  while (!iv.empty()) {
    if (iv.size() < sizeof(std::uint32_t)) {
      return Status::Corruption("sstable index truncated");
    }
    const std::uint32_t key_len = DecodeFixed32(iv);
    iv.remove_prefix(sizeof(std::uint32_t));
    if (iv.size() <
        static_cast<std::size_t>(key_len) + sizeof(std::uint64_t) + sizeof(std::uint32_t)) {
      return Status::Corruption("sstable index entry truncated");
    }
    std::string first_key(iv.substr(0, key_len));
    iv.remove_prefix(key_len);
    const std::uint64_t offset = DecodeFixed64(iv);
    iv.remove_prefix(sizeof(std::uint64_t));
    const std::uint32_t size = DecodeFixed32(iv);
    iv.remove_prefix(sizeof(std::uint32_t));
    index_.push_back(IndexEntry{std::move(first_key), offset, size});
  }
  return Status::Ok();
}

Status SSTableReader::Get(std::string_view key, std::string* value, GetResult* result) const {
  *result = GetResult::kNotPresent;
  if (index_.empty()) {
    return Status::Ok();
  }

  // The bloom filter answers "definitely absent" without touching the disk; a
  // false positive here only costs one wasted block read below.
  if (!bloom_.MayContain(key)) {
    return Status::Ok();
  }

  // Binary search for the last block whose first key is <= the target: that is
  // the only block that can contain the key (the sparse index guarantees every
  // key in block i falls in [first_key_i, first_key_{i+1})).
  std::size_t lo = 0;
  std::size_t hi = index_.size();
  while (lo < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (index_[mid].first_key <= key) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo == 0) {
    return Status::Ok();  // key precedes the first block
  }
  const IndexEntry& block_ref = index_[lo - 1];

  DataBlockReadCounter().fetch_add(1);
  // Reuse a per-thread buffer: one lookup per thread reads into the same
  // storage, avoiding a per-lookup allocation and the resize zero-fill. It only
  // grows (and zero-fills) when a larger block than any seen so far appears.
  thread_local std::string block;
  if (block.size() < block_ref.size) {
    block.resize(block_ref.size);
  }
  Status s = PreadInto(fd_, block_ref.offset, block_ref.size, &block);
  if (!s.ok()) {
    return s;
  }

  std::string_view cursor(block.data(), block_ref.size);
  while (!cursor.empty()) {
    std::string_view entry_key;
    ValueTag tag = ValueTag::kValue;
    std::string_view entry_value;
    if (!ParseEntry(&cursor, &entry_key, &tag, &entry_value)) {
      return Status::Corruption("sstable data block malformed");
    }
    if (entry_key == key) {
      if (tag == ValueTag::kTombstone) {
        *result = GetResult::kDeleted;
      } else {
        value->assign(entry_value);
        *result = GetResult::kFound;
      }
      return Status::Ok();
    }
    if (entry_key > key) {
      break;  // entries are sorted; we have passed where the key would be
    }
  }
  return Status::Ok();  // not present in its block
}

Status SSTableReader::ReadDataBlock(std::size_t block, std::string* out) const {
  const IndexEntry& entry = index_[block];
  return PreadExact(fd_, entry.offset, entry.size, out);
}

SSTableIterator::SSTableIterator(std::shared_ptr<const SSTableReader> reader)
    : reader_(std::move(reader)) {}

void SSTableIterator::SeekToFirst() {
  valid_ = false;
  status_ = Status::Ok();
  if (reader_->NumDataBlocks() == 0) {
    return;
  }
  block_index_ = 0;
  status_ = reader_->ReadDataBlock(0, &block_);
  if (!status_.ok()) {
    return;
  }
  cursor_ = block_;
  Advance();
}

void SSTableIterator::Next() { Advance(); }

void SSTableIterator::Advance() {
  while (cursor_.empty()) {
    if (block_index_ + 1 >= reader_->NumDataBlocks()) {
      valid_ = false;
      return;
    }
    ++block_index_;
    status_ = reader_->ReadDataBlock(block_index_, &block_);
    if (!status_.ok()) {
      valid_ = false;
      return;
    }
    cursor_ = block_;
  }
  if (!ParseEntry(&cursor_, &key_, &tag_, &value_)) {
    status_ = Status::Corruption("sstable data block malformed during scan");
    valid_ = false;
    return;
  }
  valid_ = true;
}

}  // namespace lsm
