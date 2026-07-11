#include "coding.h"

#include <cstddef>

namespace lsm {

void PutFixed32(std::string* dst, std::uint32_t value) {
  dst->push_back(static_cast<char>(value & 0xFFU));
  dst->push_back(static_cast<char>((value >> 8U) & 0xFFU));
  dst->push_back(static_cast<char>((value >> 16U) & 0xFFU));
  dst->push_back(static_cast<char>((value >> 24U) & 0xFFU));
}

void PutFixed64(std::string* dst, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    dst->push_back(static_cast<char>((value >> static_cast<unsigned>(shift)) & 0xFFU));
  }
}

std::uint32_t DecodeFixed32(std::string_view src) {
  return static_cast<std::uint32_t>(static_cast<unsigned char>(src[0])) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(src[1])) << 8U) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(src[2])) << 16U) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(src[3])) << 24U);
}

std::uint64_t DecodeFixed64(std::string_view src) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |=
        static_cast<std::uint64_t>(static_cast<unsigned char>(src[static_cast<std::size_t>(i)]))
        << static_cast<unsigned>(i * 8);
  }
  return value;
}

}  // namespace lsm
