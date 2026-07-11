#pragma once

#include <string>
#include <string_view>

#include "lsm/db.h"

namespace lsm {

// Writes contents to path (creating/truncating), fsyncs the file, and fsyncs
// its parent directory so both the data and the directory entry are durable.
// Used for a brand-new sstable, whose name is only referenced by the manifest
// after this returns.
Status WriteFileSync(const std::string& path, std::string_view contents);

// Durably and atomically replaces path with contents: writes a sibling temp
// file, fsyncs it, renames it over path (an atomic operation on posix), then
// fsyncs the directory. A crash leaves either the old file or the new one
// fully intact, never a half-written mix. Used for the manifest.
Status WriteFileAtomic(const std::string& path, std::string_view contents);

}  // namespace lsm
