#include "manifest.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using lsm::ManifestState;

class ManifestTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = fs::temp_directory_path() / (std::string("lsm_manifest_") + info->name());
    fs::remove_all(dir_);
    fs::create_directories(dir_);
    path_ = (dir_ / "MANIFEST").string();
  }

  void TearDown() override { fs::remove_all(dir_); }

  fs::path dir_;
  std::string path_;
};

TEST_F(ManifestTest, MissingManifestIsDefaultState) {
  ManifestState s;
  ASSERT_TRUE(lsm::ReadManifest(path_, &s).ok());
  EXPECT_EQ(s.next_file_number, 1U);
  EXPECT_TRUE(s.table_numbers.empty());
}

TEST_F(ManifestTest, RoundTripPreservesOrder) {
  ManifestState in;
  in.next_file_number = 7;
  in.table_numbers = {6, 4, 2};
  ASSERT_TRUE(lsm::WriteManifest(path_, in).ok());

  ManifestState out;
  ASSERT_TRUE(lsm::ReadManifest(path_, &out).ok());
  EXPECT_EQ(out.next_file_number, 7U);
  EXPECT_EQ(out.table_numbers, (std::vector<std::uint64_t>{6, 4, 2}));
}

TEST_F(ManifestTest, AtomicReplaceKeepsLatestAndLeavesNoTemp) {
  ManifestState v1;
  v1.next_file_number = 2;
  v1.table_numbers = {1};
  ASSERT_TRUE(lsm::WriteManifest(path_, v1).ok());

  ManifestState v2;
  v2.next_file_number = 3;
  v2.table_numbers = {2, 1};
  ASSERT_TRUE(lsm::WriteManifest(path_, v2).ok());

  ManifestState out;
  ASSERT_TRUE(lsm::ReadManifest(path_, &out).ok());
  EXPECT_EQ(out.next_file_number, 3U);
  EXPECT_EQ(out.table_numbers, (std::vector<std::uint64_t>{2, 1}));
  EXPECT_FALSE(fs::exists(path_ + ".tmp"));
}

}  // namespace
