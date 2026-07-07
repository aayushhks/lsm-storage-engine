#include "crc32.h"

#include <array>
#include <cstdint>

namespace lsm {
namespace {

// Reflected form of the ieee 802.3 polynomial. Bit-reversed processing (lsb
// first) matches zlib and lets us build a byte-indexed table.
constexpr std::uint32_t kPolynomial = 0xEDB88320U;

// Builds the 256-entry lookup table at compile time: entry i is the crc of a
// single byte i fed through the polynomial.
constexpr std::array<std::uint32_t, 256> MakeTable() {
  std::array<std::uint32_t, 256> table{};
  std::uint32_t index = 0;
  for (std::uint32_t& entry : table) {
    std::uint32_t crc = index;
    for (int bit = 0; bit < 8; ++bit) {
      crc = ((crc & 1U) != 0U) ? (kPolynomial ^ (crc >> 1U)) : (crc >> 1U);
    }
    entry = crc;
    ++index;
  }
  return table;
}

constexpr std::array<std::uint32_t, 256> kTable = MakeTable();

}  // namespace

std::uint32_t Crc32(std::string_view data) {
  std::uint32_t crc = 0xFFFFFFFFU;
  for (const char c : data) {
    const auto byte = static_cast<unsigned char>(c);
    const std::uint32_t slot = (crc ^ byte) & 0xFFU;  // provably in [0, 256)
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
    crc = kTable[slot] ^ (crc >> 8U);
  }
  return crc ^ 0xFFFFFFFFU;
}

}  // namespace lsm
