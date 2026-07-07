#include "crc32.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace {

TEST(Crc32Test, EmptyInputIsZero) { EXPECT_EQ(lsm::Crc32(""), 0x00000000U); }

TEST(Crc32Test, CanonicalCheckValue) {
  // the standard crc-32 check value for the ascii string "123456789".
  EXPECT_EQ(lsm::Crc32("123456789"), 0xCBF43926U);
}

TEST(Crc32Test, IsDeterministic) {
  EXPECT_EQ(lsm::Crc32("the quick brown fox"), lsm::Crc32("the quick brown fox"));
}

TEST(Crc32Test, DetectsSingleBitFlip) {
  const std::string a = "the quick brown fox";
  std::string b = a;
  b[0] = static_cast<char>(static_cast<unsigned char>(b[0]) ^ 0x01U);
  EXPECT_NE(lsm::Crc32(a), lsm::Crc32(b));
}

TEST(Crc32Test, HandlesEmbeddedNulBytes) {
  const std::string with_nul("a\0b\0c", 5);
  EXPECT_NE(lsm::Crc32(with_nul), 0U);
}

}  // namespace
