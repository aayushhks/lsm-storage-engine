#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "db_impl.h"
#include "lsm/db.h"

namespace {

namespace fs = std::filesystem;

lsm::Options SmallOptions() {
  lsm::Options options;
  options.memtable_flush_threshold_bytes = 1;  // flush essentially per write
  options.compaction_min_merge = 4;
  return options;
}

class CompactionDBTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = fs::temp_directory_path() / (std::string("lsm_stress_") + info->name());
    fs::remove_all(dir_);
    fs::create_directories(dir_);
  }

  void TearDown() override { fs::remove_all(dir_); }

  fs::path dir_;
};

TEST_F(CompactionDBTest, CompactionReducesTableCountAndPreservesData) {
  lsm::DBImpl db(SmallOptions(), dir_.string());
  ASSERT_TRUE(db.Recover().ok());

  const int n = 40;
  for (int i = 0; i < n; ++i) {
    ASSERT_TRUE(db.Put("key" + std::to_string(i), "val" + std::to_string(i)).ok());
  }
  db.TEST_WaitForCompaction();

  EXPECT_LT(db.TEST_TableCount(), static_cast<std::size_t>(n));  // tables were merged
  EXPECT_GE(db.TEST_TableCount(), 1U);

  for (int i = 0; i < n; ++i) {
    std::string v;
    ASSERT_TRUE(db.Get("key" + std::to_string(i), &v).ok());
    EXPECT_EQ(v, "val" + std::to_string(i));
  }
  ASSERT_TRUE(db.Close().ok());
}

TEST_F(CompactionDBTest, CompactedDataSurvivesReopen) {
  {
    lsm::DBImpl db(SmallOptions(), dir_.string());
    ASSERT_TRUE(db.Recover().ok());
    for (int i = 0; i < 40; ++i) {
      ASSERT_TRUE(db.Put("k" + std::to_string(i), "v" + std::to_string(i)).ok());
    }
    db.TEST_WaitForCompaction();
    ASSERT_TRUE(db.Close().ok());
  }
  lsm::DBImpl db(SmallOptions(), dir_.string());
  ASSERT_TRUE(db.Recover().ok());
  for (int i = 0; i < 40; ++i) {
    std::string v;
    ASSERT_TRUE(db.Get("k" + std::to_string(i), &v).ok());
    EXPECT_EQ(v, "v" + std::to_string(i));
  }
  ASSERT_TRUE(db.Close().ok());
}

// Hammer concurrent reads and writes while flushes and compactions run in the
// background. Correctness invariant: keys 0..N are always present (only ever
// overwritten, never deleted). Run under ThreadSanitizer in ci.
TEST_F(CompactionDBTest, ConcurrentReadsAndWritesDuringCompaction) {
  lsm::DBImpl db(SmallOptions(), dir_.string());
  ASSERT_TRUE(db.Recover().ok());

  const int keys = 100;
  for (int i = 0; i < keys; ++i) {
    ASSERT_TRUE(db.Put("k" + std::to_string(i), "v0").ok());
  }

  std::atomic<bool> stop{false};
  std::atomic<int> errors{0};

  std::vector<std::thread> readers;
  readers.reserve(4);
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&db, &stop, &errors, t] {
      std::mt19937 rng(static_cast<unsigned>(t) + 1);
      while (!stop.load()) {
        const int i = static_cast<int>(rng() % static_cast<unsigned>(keys));
        std::string value;
        const lsm::Status s = db.Get("k" + std::to_string(i), &value);
        if (!s.ok()) {
          errors.fetch_add(1);  // every key stays present, so a miss is a bug
        }
      }
    });
  }

  std::thread writer([&db] {
    for (int round = 1; round <= 10; ++round) {
      for (int i = 0; i < keys; ++i) {
        db.Put("k" + std::to_string(i), "v" + std::to_string(round));
      }
    }
  });

  writer.join();
  stop.store(true);
  for (std::thread& r : readers) {
    r.join();
  }

  EXPECT_EQ(errors.load(), 0);

  db.TEST_WaitForCompaction();
  for (int i = 0; i < keys; ++i) {
    std::string value;
    ASSERT_TRUE(db.Get("k" + std::to_string(i), &value).ok());
  }
  ASSERT_TRUE(db.Close().ok());
}

}  // namespace
