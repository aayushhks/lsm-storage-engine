// Placeholder benchmark entry point. The real workload harness
// (fill-sequential, fill-random, read-random uniform/zipfian, mixed) lands in
// m7. For now this is a build-and-run smoke so the bench target is held to the
// same warnings and sanitizers as the engine from day one.

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#include "lsm/db.h"

int main() {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "lsm_bench_smoke";
  std::error_code ec;
  fs::remove_all(dir, ec);

  const lsm::Options options;
  std::unique_ptr<lsm::DB> db;
  lsm::Status s = lsm::DB::Open(options, dir.string(), &db);
  if (!s.ok()) {
    std::fprintf(stderr, "open failed: %s\n", s.ToString().c_str());
    return 1;
  }

  s = db->Put("hello", "world");
  if (!s.ok()) {
    std::fprintf(stderr, "put failed: %s\n", s.ToString().c_str());
    return 1;
  }

  std::string value;
  s = db->Get("hello", &value);
  if (!s.ok()) {
    std::fprintf(stderr, "get failed: %s\n", s.ToString().c_str());
    return 1;
  }

  std::printf("lsm bench smoke ok: hello -> %s\n", value.c_str());
  fs::remove_all(dir, ec);
  return 0;
}
