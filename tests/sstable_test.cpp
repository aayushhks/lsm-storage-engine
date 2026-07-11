#include "sstable.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "skiplist.h"  // ValueTag

namespace {

namespace fs = std::filesystem;
using lsm::SSTableBuilder;
using lsm::SSTableReader;
using lsm::ValueTag;

std::string PaddedKey(int i) {
  std::string digits = std::to_string(i);
  return "key" + std::string(5 - digits.size(), '0') + digits;
}

class SSTableTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    path_ =
        (fs::temp_directory_path() / (std::string("lsm_sst_") + info->name() + ".sst")).string();
    fs::remove(path_);
  }

  void TearDown() override { fs::remove(path_); }

  void WriteTable(const std::string& bytes) {
    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }

  std::shared_ptr<SSTableReader> OpenReader() {
    std::shared_ptr<SSTableReader> r;
    EXPECT_TRUE(SSTableReader::Open(path_, &r).ok());
    return r;
  }

  std::string path_;
};

TEST_F(SSTableTest, BuildAndReadBack) {
  SSTableBuilder b;
  b.Add("alpha", ValueTag::kValue, "1");
  b.Add("bravo", ValueTag::kValue, "2");
  b.Add("charlie", ValueTag::kTombstone, "");
  WriteTable(b.Finish());
  EXPECT_EQ(b.NumEntries(), 3U);

  auto r = OpenReader();
  ASSERT_NE(r, nullptr);

  std::string v;
  SSTableReader::GetResult res = SSTableReader::GetResult::kNotPresent;
  ASSERT_TRUE(r->Get("alpha", &v, &res).ok());
  EXPECT_EQ(res, SSTableReader::GetResult::kFound);
  EXPECT_EQ(v, "1");

  ASSERT_TRUE(r->Get("charlie", &v, &res).ok());
  EXPECT_EQ(res, SSTableReader::GetResult::kDeleted);

  ASSERT_TRUE(r->Get("delta", &v, &res).ok());  // after last key
  EXPECT_EQ(res, SSTableReader::GetResult::kNotPresent);

  ASSERT_TRUE(r->Get("aardvark", &v, &res).ok());  // before first key
  EXPECT_EQ(res, SSTableReader::GetResult::kNotPresent);
}

TEST_F(SSTableTest, SpansMultipleBlocks) {
  SSTableBuilder b(64);  // tiny blocks force many, exercising the sparse index
  const int count = 500;
  for (int i = 0; i < count; ++i) {
    b.Add(PaddedKey(i), ValueTag::kValue, "v-" + PaddedKey(i));
  }
  WriteTable(b.Finish());

  auto r = OpenReader();
  for (int i = 0; i < count; ++i) {
    std::string v;
    SSTableReader::GetResult res = SSTableReader::GetResult::kNotPresent;
    ASSERT_TRUE(r->Get(PaddedKey(i), &v, &res).ok());
    EXPECT_EQ(res, SSTableReader::GetResult::kFound);
    EXPECT_EQ(v, "v-" + PaddedKey(i));
  }
  std::string v;
  SSTableReader::GetResult res = SSTableReader::GetResult::kFound;
  ASSERT_TRUE(r->Get("key99999", &v, &res).ok());
  EXPECT_EQ(res, SSTableReader::GetResult::kNotPresent);
}

TEST_F(SSTableTest, EmptyTableReadsAsNotPresent) {
  SSTableBuilder b;
  WriteTable(b.Finish());  // no entries
  auto r = OpenReader();
  std::string v;
  SSTableReader::GetResult res = SSTableReader::GetResult::kFound;
  ASSERT_TRUE(r->Get("anything", &v, &res).ok());
  EXPECT_EQ(res, SSTableReader::GetResult::kNotPresent);
}

TEST_F(SSTableTest, HandlesByteStringsWithNul) {
  SSTableBuilder b;
  const std::string key("k\0k", 3);
  const std::string val("v\0v", 3);
  b.Add(key, ValueTag::kValue, val);
  WriteTable(b.Finish());
  auto r = OpenReader();
  std::string v;
  SSTableReader::GetResult res = SSTableReader::GetResult::kNotPresent;
  ASSERT_TRUE(r->Get(key, &v, &res).ok());
  EXPECT_EQ(res, SSTableReader::GetResult::kFound);
  EXPECT_EQ(v, val);
}

TEST_F(SSTableTest, RejectsBadMagic) {
  SSTableBuilder b;
  b.Add("a", ValueTag::kValue, "1");
  std::string bytes = b.Finish();
  bytes.back() = static_cast<char>(static_cast<unsigned char>(bytes.back()) ^ 0xFFU);
  WriteTable(bytes);
  std::shared_ptr<SSTableReader> r;
  EXPECT_FALSE(SSTableReader::Open(path_, &r).ok());
}

TEST_F(SSTableTest, RejectsTooSmallFile) {
  WriteTable("short");
  std::shared_ptr<SSTableReader> r;
  EXPECT_FALSE(SSTableReader::Open(path_, &r).ok());
}

}  // namespace
