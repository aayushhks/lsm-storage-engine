#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace lsm {

// Little-endian fixed-width integer coding for the on-disk formats (the wal
// now, the sstable later). Byte-wise, so the encoding is independent of host
// endianness.

void PutFixed32(std::string* dst, std::uint32_t value);

// Reads a 32-bit little-endian value from the first four bytes of src.
// Precondition: src.size() >= 4.
std::uint32_t DecodeFixed32(std::string_view src);

}  // namespace lsm
