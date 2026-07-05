#include "lsm/db.h"

#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#include "db_impl.h"

namespace lsm {

Status DB::Open(const Options& options, const std::string& dir, std::unique_ptr<DB>* handle) {
  if (handle == nullptr) {
    return Status::InvalidArgument("null handle out-pointer");
  }
  if (dir.empty()) {
    return Status::InvalidArgument("empty directory path");
  }

  namespace fs = std::filesystem;
  std::error_code ec;
  const bool exists = fs::exists(dir, ec);
  if (ec) {
    return Status::IOError("stat '" + dir + "': " + ec.message());
  }

  if (!exists) {
    if (!options.create_if_missing) {
      return Status::InvalidArgument("directory does not exist and create_if_missing is false: " +
                                     dir);
    }
    fs::create_directories(dir, ec);
    if (ec) {
      return Status::IOError("create '" + dir + "': " + ec.message());
    }
  } else if (!fs::is_directory(dir, ec)) {
    return Status::InvalidArgument("path exists and is not a directory: " + dir);
  }

  *handle = std::make_unique<DBImpl>(dir);
  return Status::Ok();
}

}  // namespace lsm
