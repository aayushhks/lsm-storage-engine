#include "wal.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>

#include "coding.h"
#include "crc32.h"

namespace lsm {
namespace {

constexpr std::size_t kHeaderSize = 8;               // u32 length + u32 crc
constexpr std::uint64_t kMaxRecord = 0xFFFFFFFFULL;  // payload length fits u32

Status PosixError(const std::string& context) {
  const std::error_code ec(errno, std::generic_category());
  return Status::IOError(context + ": " + ec.message());
}

void EncodeMutation(std::string* dst, ValueTag tag, std::string_view key, std::string_view value) {
  dst->push_back(static_cast<char>(static_cast<std::uint8_t>(tag)));
  PutFixed32(dst, static_cast<std::uint32_t>(key.size()));
  dst->append(key);
  PutFixed32(dst, static_cast<std::uint32_t>(value.size()));
  dst->append(value);
}

// Parses one mutation payload. Returns false on any structural inconsistency;
// the crc has already vouched for the bytes, so this is defensive.
bool DecodeMutation(std::string_view payload, ValueTag* tag, std::string_view* key,
                    std::string_view* value) {
  if (payload.size() < 1 + sizeof(std::uint32_t)) {
    return false;
  }
  const auto type = static_cast<unsigned char>(payload[0]);
  payload.remove_prefix(1);

  const std::uint32_t key_len = DecodeFixed32(payload);
  payload.remove_prefix(sizeof(std::uint32_t));
  if (payload.size() < key_len) {
    return false;
  }
  *key = payload.substr(0, key_len);
  payload.remove_prefix(key_len);

  if (payload.size() < sizeof(std::uint32_t)) {
    return false;
  }
  const std::uint32_t value_len = DecodeFixed32(payload);
  payload.remove_prefix(sizeof(std::uint32_t));
  if (payload.size() < value_len) {
    return false;
  }
  *value = payload.substr(0, value_len);
  payload.remove_prefix(value_len);

  if (!payload.empty()) {
    return false;  // trailing bytes
  }
  if (type == static_cast<unsigned char>(ValueTag::kValue)) {
    *tag = ValueTag::kValue;
  } else if (type == static_cast<unsigned char>(ValueTag::kTombstone)) {
    *tag = ValueTag::kTombstone;
  } else {
    return false;
  }
  return true;
}

// Fsyncs the directory containing path so a freshly created log file's
// directory entry is itself durable, not just the file's data.
Status SyncParentDirectory(const std::string& path) {
  const std::filesystem::path parent = std::filesystem::path(path).parent_path();
  const std::string dir = parent.empty() ? std::string(".") : parent.string();
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) posix open is variadic by declaration
  const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
  if (dfd < 0) {
    return PosixError("open dir " + dir);
  }
  const int rc = ::fsync(dfd);
  ::close(dfd);
  if (rc != 0) {
    return PosixError("fsync dir " + dir);
  }
  return Status::Ok();
}

}  // namespace

Status WalWriter::Open(const std::string& path, std::unique_ptr<WalWriter>* out) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) posix open is variadic by declaration
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0) {
    return PosixError("open wal " + path);
  }
  Status s = SyncParentDirectory(path);
  if (!s.ok()) {
    ::close(fd);
    return s;
  }
  *out = std::make_unique<WalWriter>(fd, path);
  return Status::Ok();
}

WalWriter::WalWriter(int fd, std::string path) : fd_(fd), path_(std::move(path)) {}

WalWriter::~WalWriter() {
  if (fd_ >= 0) {
    static_cast<void>(::fsync(fd_));
    static_cast<void>(::close(fd_));
    fd_ = -1;
  }
}

Status WalWriter::WriteAll(std::string_view bytes) {
  std::size_t written = 0;
  while (written < bytes.size()) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic) advance past a partial write
    const ssize_t n = ::write(fd_, bytes.data() + written, bytes.size() - written);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return PosixError("write " + path_);
    }
    written += static_cast<std::size_t>(n);
  }
  return Status::Ok();
}

Status WalWriter::Append(ValueTag tag, std::string_view key, std::string_view value) {
  std::string payload;
  EncodeMutation(&payload, tag, key, value);
  if (payload.size() > kMaxRecord) {
    return Status::InvalidArgument("record too large for the wal frame");
  }

  std::string frame;
  PutFixed32(&frame, static_cast<std::uint32_t>(payload.size()));
  PutFixed32(&frame, Crc32(payload));
  frame.append(payload);

  Status s = WriteAll(frame);
  if (!s.ok()) {
    return s;
  }
  if (::fsync(fd_) != 0) {
    return PosixError("fsync " + path_);
  }
  return Status::Ok();
}

Status WalWriter::Close() {
  if (fd_ < 0) {
    return Status::Ok();
  }
  const int sync_rc = ::fsync(fd_);
  const int close_rc = ::close(fd_);
  fd_ = -1;
  if (sync_rc != 0 || close_rc != 0) {
    return Status::IOError("close " + path_);
  }
  return Status::Ok();
}

Status RecoverWal(const std::string& path, const WalApplyFn& apply) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return Status::Ok();  // nothing to replay
  }

  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return Status::IOError("open wal for recovery: " + path);
  }
  const std::string buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const std::string_view all(buf);

  std::size_t offset = 0;
  std::size_t good = 0;  // byte offset just past the last valid record
  while (offset + kHeaderSize <= all.size()) {
    const std::string_view header = all.substr(offset, kHeaderSize);
    const std::uint32_t len = DecodeFixed32(header);
    const std::uint32_t crc = DecodeFixed32(header.substr(sizeof(std::uint32_t)));
    if (all.size() - offset - kHeaderSize < len) {
      break;  // torn tail: payload incomplete
    }
    const std::string_view payload = all.substr(offset + kHeaderSize, len);
    if (Crc32(payload) != crc) {
      break;  // corrupt tail: checksum mismatch
    }

    ValueTag tag = ValueTag::kValue;
    std::string_view key;
    std::string_view value;
    if (!DecodeMutation(payload, &tag, &key, &value)) {
      break;  // structurally malformed despite a valid crc
    }
    apply(tag, key, value);
    offset += kHeaderSize + len;
    good = offset;
  }

  if (good < all.size()) {
    // drop the torn/corrupt tail so subsequent appends start from a clean edge.
    std::filesystem::resize_file(path, good, ec);
    if (ec) {
      return Status::IOError("truncate wal: " + ec.message());
    }
  }
  return Status::Ok();
}

}  // namespace lsm
