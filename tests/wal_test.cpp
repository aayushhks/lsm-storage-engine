#include "wal.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "skiplist.h"  // ValueTag

namespace {

namespace fs = std::filesystem;
using lsm::ValueTag;

struct Record {
  ValueTag tag;
  std::string key;
  std::string value;
};

class WalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    path_ =
        (fs::temp_directory_path() / (std::string("lsm_wal_") + info->name() + ".log")).string();
    fs::remove(path_);
  }

  void TearDown() override { fs::remove(path_); }

  std::vector<Record> Recover() {
    std::vector<Record> out;
    const lsm::Status s =
        lsm::RecoverWal(path_, [&out](ValueTag tag, std::string_view key, std::string_view value) {
          out.push_back({tag, std::string(key), std::string(value)});
        });
    EXPECT_TRUE(s.ok()) << s.ToString();
    return out;
  }

  std::string ReadFile() const {
    std::ifstream in(path_, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }

  void WriteFile(const std::string& data) const {
    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
  }

  std::string path_;
};

TEST_F(WalTest, RecoverMissingFileIsOk) { EXPECT_TRUE(Recover().empty()); }

TEST_F(WalTest, AppendThenRecoverPreservesOrderAndTags) {
  {
    std::unique_ptr<lsm::WalWriter> w;
    ASSERT_TRUE(lsm::WalWriter::Open(path_, &w).ok());
    ASSERT_TRUE(w->Append(ValueTag::kValue, "a", "1").ok());
    ASSERT_TRUE(w->Append(ValueTag::kValue, "b", "22").ok());
    ASSERT_TRUE(w->Append(ValueTag::kTombstone, "a", "").ok());
    ASSERT_TRUE(w->Close().ok());
  }
  const auto records = Recover();
  ASSERT_EQ(records.size(), 3U);
  EXPECT_EQ(records[0].key, "a");
  EXPECT_EQ(records[0].value, "1");
  EXPECT_EQ(records[0].tag, ValueTag::kValue);
  EXPECT_EQ(records[1].key, "b");
  EXPECT_EQ(records[1].value, "22");
  EXPECT_EQ(records[2].key, "a");
  EXPECT_EQ(records[2].tag, ValueTag::kTombstone);
}

TEST_F(WalTest, RecoverHandlesByteStringsWithNul) {
  const std::string key("k\0k", 3);
  const std::string value("v\0v\0v", 5);
  {
    std::unique_ptr<lsm::WalWriter> w;
    ASSERT_TRUE(lsm::WalWriter::Open(path_, &w).ok());
    ASSERT_TRUE(w->Append(ValueTag::kValue, key, value).ok());
    ASSERT_TRUE(w->Close().ok());
  }
  const auto records = Recover();
  ASSERT_EQ(records.size(), 1U);
  EXPECT_EQ(records[0].key, key);
  EXPECT_EQ(records[0].value, value);
}

TEST_F(WalTest, CorruptTailRecordIsDroppedAndTruncated) {
  {
    std::unique_ptr<lsm::WalWriter> w;
    ASSERT_TRUE(lsm::WalWriter::Open(path_, &w).ok());
    ASSERT_TRUE(w->Append(ValueTag::kValue, "keep1", "v1").ok());
    ASSERT_TRUE(w->Append(ValueTag::kValue, "keep2", "v2").ok());
    ASSERT_TRUE(w->Append(ValueTag::kValue, "torn", "v3").ok());
    ASSERT_TRUE(w->Close().ok());
  }
  const auto size_before = fs::file_size(path_);

  // flip the final byte (inside the last record's payload) to break its crc.
  std::string data = ReadFile();
  ASSERT_FALSE(data.empty());
  data.back() = static_cast<char>(static_cast<unsigned char>(data.back()) ^ 0xFFU);
  WriteFile(data);

  const auto records = Recover();
  ASSERT_EQ(records.size(), 2U);
  EXPECT_EQ(records[0].key, "keep1");
  EXPECT_EQ(records[1].key, "keep2");
  EXPECT_LT(fs::file_size(path_), size_before);  // torn tail truncated away
}

TEST_F(WalTest, PartialTailBytesAreTruncated) {
  {
    std::unique_ptr<lsm::WalWriter> w;
    ASSERT_TRUE(lsm::WalWriter::Open(path_, &w).ok());
    ASSERT_TRUE(w->Append(ValueTag::kValue, "x", "1").ok());
    ASSERT_TRUE(w->Close().ok());
  }
  const auto good_size = fs::file_size(path_);

  // append a few stray bytes that cannot form a complete record header.
  {
    std::ofstream f(path_, std::ios::binary | std::ios::app);
    const std::string junk("\x05\x00\x00", 3);  // partial length field
    f.write(junk.data(), static_cast<std::streamsize>(junk.size()));
  }
  const auto records = Recover();
  ASSERT_EQ(records.size(), 1U);
  EXPECT_EQ(records[0].key, "x");
  EXPECT_EQ(fs::file_size(path_), good_size);  // stray bytes removed
}

TEST_F(WalTest, AppendsAfterRecoveryTruncationAreContiguous) {
  {
    std::unique_ptr<lsm::WalWriter> w;
    ASSERT_TRUE(lsm::WalWriter::Open(path_, &w).ok());
    ASSERT_TRUE(w->Append(ValueTag::kValue, "first", "1").ok());
    ASSERT_TRUE(w->Close().ok());
  }
  // corrupt a trailing partial record, recover (truncates), then append again.
  {
    std::ofstream f(path_, std::ios::binary | std::ios::app);
    const std::string junk("\xFF\xFF", 2);
    f.write(junk.data(), static_cast<std::streamsize>(junk.size()));
  }
  ASSERT_EQ(Recover().size(), 1U);  // truncates the junk
  {
    std::unique_ptr<lsm::WalWriter> w;
    ASSERT_TRUE(lsm::WalWriter::Open(path_, &w).ok());
    ASSERT_TRUE(w->Append(ValueTag::kValue, "second", "2").ok());
    ASSERT_TRUE(w->Close().ok());
  }
  const auto records = Recover();
  ASSERT_EQ(records.size(), 2U);
  EXPECT_EQ(records[0].key, "first");
  EXPECT_EQ(records[1].key, "second");
}

}  // namespace
