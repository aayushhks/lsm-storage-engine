#include "file_util.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>

namespace lsm {
namespace {

Status PosixError(const std::string& context) {
  const std::error_code ec(errno, std::generic_category());
  return Status::IOError(context + ": " + ec.message());
}

Status WriteAllToFd(int fd, std::string_view bytes, const std::string& path) {
  std::size_t written = 0;
  while (written < bytes.size()) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic) advance past a partial write
    const ssize_t n = ::write(fd, bytes.data() + written, bytes.size() - written);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return PosixError("write " + path);
    }
    written += static_cast<std::size_t>(n);
  }
  return Status::Ok();
}

Status SyncDirectory(const std::filesystem::path& dir) {
  const std::string dir_str = dir.empty() ? std::string(".") : dir.string();
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) posix open is variadic by declaration
  const int dfd = ::open(dir_str.c_str(), O_RDONLY | O_DIRECTORY);
  if (dfd < 0) {
    return PosixError("open dir " + dir_str);
  }
  const int rc = ::fsync(dfd);
  ::close(dfd);
  if (rc != 0) {
    return PosixError("fsync dir " + dir_str);
  }
  return Status::Ok();
}

// Writes contents to path and fsyncs the file. The caller fsyncs the directory.
Status WriteAndSyncFile(const std::string& path, std::string_view contents) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) posix open is variadic by declaration
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return PosixError("open " + path);
  }
  Status s = WriteAllToFd(fd, contents, path);
  if (s.ok() && ::fsync(fd) != 0) {
    s = PosixError("fsync " + path);
  }
  if (::close(fd) != 0 && s.ok()) {
    s = PosixError("close " + path);
  }
  return s;
}

}  // namespace

Status WriteFileSync(const std::string& path, std::string_view contents) {
  Status s = WriteAndSyncFile(path, contents);
  if (!s.ok()) {
    return s;
  }
  return SyncDirectory(std::filesystem::path(path).parent_path());
}

Status WriteFileAtomic(const std::string& path, std::string_view contents) {
  const std::string tmp = path + ".tmp";
  Status s = WriteAndSyncFile(tmp, contents);
  if (!s.ok()) {
    return s;
  }

  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::filesystem::remove(tmp, ec);
    return Status::IOError("rename " + tmp + " -> " + path + ": " + ec.message());
  }
  return SyncDirectory(std::filesystem::path(path).parent_path());
}

}  // namespace lsm
