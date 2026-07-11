#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "lsm/db.h"

namespace lsm {

// The manifest records the durable state of the table set: the next file number
// to allocate and the live sstable numbers, newest first. It is rewritten
// atomically (write temp + fsync + rename + fsync dir), so a reader never
// observes a half-written manifest and no checksum is required.
struct ManifestState {
  std::uint64_t next_file_number = 1;
  std::vector<std::uint64_t> table_numbers;  // newest first
};

// Reads the manifest at path. A missing manifest yields the default state with
// an ok status (a fresh database).
Status ReadManifest(const std::string& path, ManifestState* out);

// Atomically writes state to the manifest at path.
Status WriteManifest(const std::string& path, const ManifestState& state);

}  // namespace lsm
