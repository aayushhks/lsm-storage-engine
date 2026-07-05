#include <utility>

#include "lsm/db.h"

namespace lsm {

Status::Status() : code_(Code::kOk) {}

Status::Status(Code code, std::string message) : code_(code), message_(std::move(message)) {}

Status Status::Ok() { return Status{}; }

Status Status::NotFound(std::string msg) { return Status{Code::kNotFound, std::move(msg)}; }

Status Status::Corruption(std::string msg) { return Status{Code::kCorruption, std::move(msg)}; }

Status Status::IOError(std::string msg) { return Status{Code::kIOError, std::move(msg)}; }

Status Status::InvalidArgument(std::string msg) {
  return Status{Code::kInvalidArgument, std::move(msg)};
}

bool Status::ok() const { return code_ == Code::kOk; }
bool Status::IsNotFound() const { return code_ == Code::kNotFound; }
bool Status::IsCorruption() const { return code_ == Code::kCorruption; }
bool Status::IsIOError() const { return code_ == Code::kIOError; }
bool Status::IsInvalidArgument() const { return code_ == Code::kInvalidArgument; }

Status::Code Status::code() const { return code_; }
const std::string& Status::message() const { return message_; }

std::string Status::ToString() const {
  std::string label;
  switch (code_) {
    case Code::kOk:
      return "ok";
    case Code::kNotFound:
      label = "not found";
      break;
    case Code::kCorruption:
      label = "corruption";
      break;
    case Code::kIOError:
      label = "io error";
      break;
    case Code::kInvalidArgument:
      label = "invalid argument";
      break;
  }
  if (message_.empty()) {
    return label;
  }
  return label + ": " + message_;
}

}  // namespace lsm
