#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "lsm/db.h"
#include "sstable.h"  // build an orphan sstable directly, read the block-read counter

namespace {

namespace fs = std::filesystem;

class FlushTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = fs::temp_directory_path() / (std::string("lsm_flush_") + info->name());
    fs::remove_all(dir_);
  }

  void TearDown() override { fs::remove_all(dir_); }

  std::unique_ptr<lsm::DB> Open(std::size_t threshold) {
    lsm::Options options;
    options.memtable_flush_threshold_bytes = threshold;
    std::unique_ptr<lsm::DB> db;
    EXPECT_TRUE(lsm::DB::Open(options, dir_.string(), &db).ok());
    return db;
  }

  std::size_t CountSstFiles() const {
    std::size_t n = 0;
    for (const auto& entry : fs::directory_iterator(dir_)) {
      if (entry.path().extension() == ".sst") {
        ++n;
      }
    }
    return n;
  }

  fs::path dir_;
};

TEST_F(FlushTest, FlushesOnThresholdAndDataStaysReadable) {
  auto db = Open(1);  // flush after essentially every write
  for (int i = 0; i < 50; ++i) {
    ASSERT_TRUE(db->Put("key" + std::to_string(i), "val" + std::to_string(i)).ok());
  }
  for (int i = 0; i < 50; ++i) {
    std::string v;
    ASSERT_TRUE(db->Get("key" + std::to_string(i), &v).ok());
    EXPECT_EQ(v, "val" + std::to_string(i));
  }
  EXPECT_GT(CountSstFiles(), 0U);
}

TEST_F(FlushTest, ReopenAfterFlushSeesAllData) {
  {
    auto db = Open(1);
    for (int i = 0; i < 50; ++i) {
      ASSERT_TRUE(db->Put("k" + std::to_string(i), "v" + std::to_string(i)).ok());
    }
    ASSERT_TRUE(db->Close().ok());
  }
  auto db = Open(1);
  for (int i = 0; i < 50; ++i) {
    std::string v;
    ASSERT_TRUE(db->Get("k" + std::to_string(i), &v).ok());
    EXPECT_EQ(v, "v" + std::to_string(i));
  }
}

TEST_F(FlushTest, DeleteShadowsFlushedValueAcrossReopen) {
  auto db = Open(1);
  ASSERT_TRUE(db->Put("k", "v").ok());  // flushes to an sstable
  std::string v;
  ASSERT_TRUE(db->Get("k", &v).ok());  // the flushed value is genuinely readable
  EXPECT_EQ(v, "v");
  ASSERT_TRUE(db->Delete("k").ok());  // tombstone shadows it
  EXPECT_TRUE(db->Get("k", &v).IsNotFound());
  ASSERT_TRUE(db->Close().ok());

  auto db2 = Open(1);
  EXPECT_TRUE(db2->Get("k", &v).IsNotFound());
}

TEST_F(FlushTest, OverwriteAcrossFlushReturnsLatest) {
  auto db = Open(1);
  ASSERT_TRUE(db->Put("k", "old").ok());
  ASSERT_TRUE(db->Put("k", "new").ok());
  std::string v;
  ASSERT_TRUE(db->Get("k", &v).ok());
  EXPECT_EQ(v, "new");
  ASSERT_TRUE(db->Close().ok());

  auto db2 = Open(1);
  ASSERT_TRUE(db2->Get("k", &v).ok());
  EXPECT_EQ(v, "new");
}

// Recovery rule: an sstable not referenced by the manifest is ignored, and the
// wal (never discarded before the manifest commit) still holds the data. This
// is exactly the crash-between-flush-and-manifest-update case.
TEST_F(FlushTest, OrphanSstableIgnoredAndWalRecovers) {
  {
    auto db = Open(1U << 30);  // no auto-flush: data stays in wal + memtable
    ASSERT_TRUE(db->Put("a", "1").ok());
    ASSERT_TRUE(db->Put("b", "2").ok());
    ASSERT_TRUE(db->Put("c", "3").ok());
    ASSERT_TRUE(db->Close().ok());
  }
  // an sstable written by a flush that crashed before committing the manifest.
  {
    lsm::SSTableBuilder b;
    b.Add("a", lsm::ValueTag::kValue, "STALE");  // wrong on purpose: must be ignored
    const std::string bytes = b.Finish();
    std::ofstream out(dir_ / "999.sst", std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  ASSERT_FALSE(fs::exists(dir_ / "MANIFEST"));

  auto db = Open(1U << 30);
  std::string v;
  ASSERT_TRUE(db->Get("a", &v).ok());
  EXPECT_EQ(v, "1");  // from the wal, not the orphan sstable
  ASSERT_TRUE(db->Get("b", &v).ok());
  EXPECT_EQ(v, "2");
  ASSERT_TRUE(db->Get("c", &v).ok());
  EXPECT_EQ(v, "3");
}

// Read amplification for a miss: with a bloom filter per table, a key absent
// from all of them should read almost no data blocks; without, it reads one per
// table. This is the measured before/after the design doc cites.
TEST_F(FlushTest, BloomFiltersCutReadAmplificationForMisses) {
  auto db = Open(1);  // one table per key
  const int tables = 100;
  for (int i = 0; i < tables; ++i) {
    ASSERT_TRUE(db->Put("k" + std::to_string(i), "v").ok());
  }
  lsm::ResetSSTableDataBlockReads();
  std::string v;
  EXPECT_TRUE(db->Get("totally-absent", &v).IsNotFound());
  const std::uint64_t with_bloom = lsm::SSTableDataBlockReads();
  EXPECT_LT(with_bloom, 10U);  // blooms skip almost every table
}

TEST_F(FlushTest, WithoutBloomEveryTableIsProbedOnAMiss) {
  lsm::Options options;
  options.memtable_flush_threshold_bytes = 1;
  options.enable_bloom_filters = false;
  std::unique_ptr<lsm::DB> db;
  ASSERT_TRUE(lsm::DB::Open(options, dir_.string(), &db).ok());
  const int tables = 100;
  for (int i = 0; i < tables; ++i) {
    ASSERT_TRUE(db->Put("k" + std::to_string(i), "v").ok());
  }
  lsm::ResetSSTableDataBlockReads();
  std::string v;
  EXPECT_TRUE(db->Get("totally-absent", &v).IsNotFound());
  const std::uint64_t without_bloom = lsm::SSTableDataBlockReads();
  EXPECT_EQ(without_bloom, static_cast<std::uint64_t>(tables));  // one block read per table
}

}  // namespace
