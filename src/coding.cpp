#include "coding.h"

namespace lsm {

void PutFixed32(std::string* dst, std::uint32_t value) {
  dst->push_back(static_cast<char>(value & 0xFFU));
  dst->push_back(static_cast<char>((value >> 8U) & 0xFFU));
  dst->push_back(static_cast<char>((value >> 16U) & 0xFFU));
  dst->push_back(static_cast<char>((value >> 24U) & 0xFFU));
}

std::uint32_t DecodeFixed32(std::string_view src) {
  return static_cast<std::uint32_t>(static_cast<unsigned char>(src[0])) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(src[1])) << 8U) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(src[2])) << 16U) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(src[3])) << 24U);
}

}  // namespace lsm
