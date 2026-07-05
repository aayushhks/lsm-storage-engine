#include "lsm/db.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

namespace {

namespace fs = std::filesystem;

// Each test gets a fresh, uniquely named directory under the system temp dir,
// wiped before and after so runs never interfere (ctest may run cases in
// parallel processes).
class DBTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = fs::temp_directory_path() /
           (std::string("lsm_test_") + info->test_suite_name() + "_" + info->name());
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  void TearDown() override {
    db_.reset();
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  lsm::Status Open() {
    const lsm::Options options;
    return lsm::DB::Open(options, dir_.string(), &db_);
  }

  fs::path dir_;
  std::unique_ptr<lsm::DB> db_;
};

TEST_F(DBTest, OpenCreatesDirectory) {
  ASSERT_TRUE(Open().ok());
  EXPECT_TRUE(fs::is_directory(dir_));
}

TEST_F(DBTest, PutThenGetReturnsValue) {
  ASSERT_TRUE(Open().ok());
  ASSERT_TRUE(db_->Put("k", "v").ok());

  std::string value;
  ASSERT_TRUE(db_->Get("k", &value).ok());
  EXPECT_EQ(value, "v");
}

TEST_F(DBTest, GetMissingKeyReturnsNotFound) {
  ASSERT_TRUE(Open().ok());

  std::string value;
  const lsm::Status s = db_->Get("absent", &value);
  EXPECT_TRUE(s.IsNotFound());
  EXPECT_FALSE(s.ok());
}

TEST_F(DBTest, PutOverwritesValue) {
  ASSERT_TRUE(Open().ok());
  ASSERT_TRUE(db_->Put("k", "v1").ok());
  ASSERT_TRUE(db_->Put("k", "v2").ok());

  std::string value;
  ASSERT_TRUE(db_->Get("k", &value).ok());
  EXPECT_EQ(value, "v2");
}

TEST_F(DBTest, DeleteRemovesKey) {
  ASSERT_TRUE(Open().ok());
  ASSERT_TRUE(db_->Put("k", "v").ok());
  ASSERT_TRUE(db_->Delete("k").ok());

  std::string value;
  EXPECT_TRUE(db_->Get("k", &value).IsNotFound());
}

TEST_F(DBTest, DeleteMissingKeyIsOk) {
  ASSERT_TRUE(Open().ok());
  EXPECT_TRUE(db_->Delete("never-existed").ok());
}

TEST_F(DBTest, KeysAndValuesAreByteStrings) {
  ASSERT_TRUE(Open().ok());
  const std::string key("a\0b", 3);
  const std::string val("x\0y\0z", 5);
  ASSERT_TRUE(db_->Put(key, val).ok());

  std::string got;
  ASSERT_TRUE(db_->Get(key, &got).ok());
  EXPECT_EQ(got, val);
  EXPECT_EQ(got.size(), 5U);
}

TEST_F(DBTest, EmptyKeyIsRejected) {
  ASSERT_TRUE(Open().ok());
  EXPECT_TRUE(db_->Put("", "v").IsInvalidArgument());
  EXPECT_TRUE(db_->Delete("").IsInvalidArgument());
}

TEST_F(DBTest, GetWithNullValuePointerIsRejected) {
  ASSERT_TRUE(Open().ok());
  EXPECT_TRUE(db_->Get("k", nullptr).IsInvalidArgument());
}

TEST_F(DBTest, OpenMissingWithoutCreateFails) {
  lsm::Options options;
  options.create_if_missing = false;
  std::unique_ptr<lsm::DB> db;
  const lsm::Status s = lsm::DB::Open(options, dir_.string(), &db);
  EXPECT_FALSE(s.ok());
  EXPECT_EQ(db, nullptr);
}

TEST_F(DBTest, CloseIsIdempotent) {
  ASSERT_TRUE(Open().ok());
  EXPECT_TRUE(db_->Close().ok());
  EXPECT_TRUE(db_->Close().ok());
}

TEST(StatusTest, ToStringIncludesMessage) {
  const lsm::Status s = lsm::Status::IOError("disk gone");
  EXPECT_TRUE(s.IsIOError());
  EXPECT_EQ(s.ToString(), "io error: disk gone");
}

TEST(StatusTest, OkHasNoMessage) {
  const lsm::Status s = lsm::Status::Ok();
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(s.ToString(), "ok");
}

}  // namespace
