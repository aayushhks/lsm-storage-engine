#pragma once

#include <cstdint>
#include <string_view>

namespace lsm {

// Standard CRC-32 (ieee 802.3: polynomial 0xedb88320 reflected, initial and
// final xor 0xffffffff). Matches zlib's crc32. Implemented from scratch with a
// precomputed lookup table; used to detect torn or corrupt wal records.
std::uint32_t Crc32(std::string_view data);

}  // namespace lsm
