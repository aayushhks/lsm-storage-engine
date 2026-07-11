#include "manifest.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

#include "coding.h"
#include "file_util.h"

namespace lsm {
namespace {

constexpr std::uint64_t kManifestMagic = 0x6C736D5F4D414E31ULL;  // "lsm_MAN1"
constexpr std::size_t kMinManifestSize = 8 + 8 + 4;              // magic + next number + count

}  // namespace

Status ReadManifest(const std::string& path, ManifestState* out) {
  *out = ManifestState{};

  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return Status::Ok();  // fresh database
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return Status::IOError("open manifest: " + path);
  }
  const std::string buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::string_view v(buf);
  if (v.size() < kMinManifestSize) {
    return Status::Corruption("manifest too small");
  }

  const std::uint64_t magic = DecodeFixed64(v);
  v.remove_prefix(sizeof(std::uint64_t));
  if (magic != kManifestMagic) {
    return Status::Corruption("manifest bad magic");
  }
  out->next_file_number = DecodeFixed64(v);
  v.remove_prefix(sizeof(std::uint64_t));
  const std::uint32_t count = DecodeFixed32(v);
  v.remove_prefix(sizeof(std::uint32_t));

  for (std::uint32_t i = 0; i < count; ++i) {
    if (v.size() < sizeof(std::uint64_t)) {
      return Status::Corruption("manifest truncated");
    }
    out->table_numbers.push_back(DecodeFixed64(v));
    v.remove_prefix(sizeof(std::uint64_t));
  }
  return Status::Ok();
}

Status WriteManifest(const std::string& path, const ManifestState& state) {
  std::string content;
  PutFixed64(&content, kManifestMagic);
  PutFixed64(&content, state.next_file_number);
  PutFixed32(&content, static_cast<std::uint32_t>(state.table_numbers.size()));
  for (const std::uint64_t number : state.table_numbers) {
    PutFixed64(&content, number);
  }
  return WriteFileAtomic(path, content);
}

}  // namespace lsm
