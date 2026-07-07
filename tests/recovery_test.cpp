#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

#include "lsm/db.h"

namespace {

namespace fs = std::filesystem;

// End-to-end crash recovery through the public api: write, drop the handle
// without a clean shutdown, reopen, and check the wal replayed correctly.
class RecoveryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = fs::temp_directory_path() / (std::string("lsm_recover_") + info->name());
    fs::remove_all(dir_);
  }

  void TearDown() override { fs::remove_all(dir_); }

  std::unique_ptr<lsm::DB> Open() {
    const lsm::Options options;
    std::unique_ptr<lsm::DB> db;
    EXPECT_TRUE(lsm::DB::Open(options, dir_.string(), &db).ok());
    return db;
  }

  fs::path dir_;
};

TEST_F(RecoveryTest, ReopenRecoversAllWrites) {
  {
    auto db = Open();
    for (int i = 0; i < 200; ++i) {
      ASSERT_TRUE(db->Put("key" + std::to_string(i), "val" + std::to_string(i)).ok());
    }
    // leave scope without Close() to mimic a crash. per-write fsync already made
    // every record durable in the wal.
  }
  auto db = Open();
  for (int i = 0; i < 200; ++i) {
    std::string value;
    ASSERT_TRUE(db->Get("key" + std::to_string(i), &value).ok());
    EXPECT_EQ(value, "val" + std::to_string(i));
  }
}

TEST_F(RecoveryTest, ReopenRecoversDeletes) {
  {
    auto db = Open();
    ASSERT_TRUE(db->Put("k", "v").ok());
    ASSERT_TRUE(db->Delete("k").ok());
  }
  auto db = Open();
  std::string value;
  EXPECT_TRUE(db->Get("k", &value).IsNotFound());
}

TEST_F(RecoveryTest, ReopenRecoversLatestOverwrite) {
  {
    auto db = Open();
    ASSERT_TRUE(db->Put("k", "first").ok());
    ASSERT_TRUE(db->Put("k", "second").ok());
  }
  auto db = Open();
  std::string value;
  ASSERT_TRUE(db->Get("k", &value).ok());
  EXPECT_EQ(value, "second");
}

TEST_F(RecoveryTest, TornTailDropsOnlyTheLastRecord) {
  {
    auto db = Open();
    ASSERT_TRUE(db->Put("keep", "yes").ok());
    ASSERT_TRUE(db->Put("torn", "maybe").ok());
  }
  // corrupt the tail of the on-disk wal.
  const fs::path wal = dir_ / "wal.log";
  std::string data;
  {
    std::ifstream in(wal, std::ios::binary);
    data.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }
  ASSERT_FALSE(data.empty());
  data.back() = static_cast<char>(static_cast<unsigned char>(data.back()) ^ 0xFFU);
  {
    std::ofstream out(wal, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
  }

  auto db = Open();  // must recover gracefully, not crash
  std::string value;
  EXPECT_TRUE(db->Get("keep", &value).ok());
  EXPECT_EQ(value, "yes");
  EXPECT_TRUE(db->Get("torn", &value).IsNotFound());  // torn record dropped
}

TEST_F(RecoveryTest, ReopenAfterCleanCloseRecovers) {
  {
    auto db = Open();
    ASSERT_TRUE(db->Put("k", "v").ok());
    ASSERT_TRUE(db->Close().ok());
  }
  auto db = Open();
  std::string value;
  ASSERT_TRUE(db->Get("k", &value).ok());
  EXPECT_EQ(value, "v");
}

}  // namespace
