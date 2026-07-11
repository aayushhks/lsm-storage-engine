#include "compaction.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "sstable.h"

namespace {

namespace fs = std::filesystem;
using lsm::SSTableBuilder;
using lsm::SSTableReader;
using lsm::ValueTag;

struct Entry {
  std::string key;
  ValueTag tag;
  std::string value;
};

Entry Val(std::string key, std::string value) {
  return Entry{std::move(key), ValueTag::kValue, std::move(value)};
}
Entry Tomb(std::string key) { return Entry{std::move(key), ValueTag::kTombstone, ""}; }

class CompactionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = fs::temp_directory_path() / (std::string("lsm_compact_") + info->name());
    fs::remove_all(dir_);
    fs::create_directories(dir_);
  }

  void TearDown() override { fs::remove_all(dir_); }

  std::string Path(const std::string& name) const { return (dir_ / name).string(); }

  std::shared_ptr<SSTableReader> MakeTable(const std::string& name,
                                           const std::vector<Entry>& entries) {
    SSTableBuilder builder;
    for (const Entry& e : entries) {
      builder.Add(e.key, e.tag, e.value);
    }
    std::ofstream out(Path(name), std::ios::binary | std::ios::trunc);
    const std::string bytes = builder.Finish();
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    std::shared_ptr<SSTableReader> reader;
    EXPECT_TRUE(SSTableReader::Open(Path(name), &reader).ok());
    return reader;
  }

  SSTableReader::GetResult Get(const std::shared_ptr<SSTableReader>& reader, const std::string& key,
                               std::string* value) {
    SSTableReader::GetResult result = SSTableReader::GetResult::kNotPresent;
    EXPECT_TRUE(reader->Get(key, value, &result).ok());
    return result;
  }

  fs::path dir_;
};

TEST_F(CompactionTest, MergesAndKeepsNewestValue) {
  auto newest = MakeTable("0.sst", {Val("a", "A"), Val("k", "new")});
  auto older = MakeTable("1.sst", {Val("k", "old"), Val("m", "M")});

  const lsm::Options options;
  ASSERT_TRUE(lsm::CompactTables({newest, older}, Path("out.sst"), options, true).ok());

  std::shared_ptr<SSTableReader> out;
  ASSERT_TRUE(SSTableReader::Open(Path("out.sst"), &out).ok());

  std::string v;
  EXPECT_EQ(Get(out, "a", &v), SSTableReader::GetResult::kFound);
  EXPECT_EQ(v, "A");
  EXPECT_EQ(Get(out, "k", &v), SSTableReader::GetResult::kFound);
  EXPECT_EQ(v, "new");  // newest table wins
  EXPECT_EQ(Get(out, "m", &v), SSTableReader::GetResult::kFound);
  EXPECT_EQ(v, "M");
}

TEST_F(CompactionTest, DropsTombstoneWhenOldestIsIncluded) {
  auto newest = MakeTable("0.sst", {Tomb("k")});
  auto older = MakeTable("1.sst", {Val("k", "old")});

  const lsm::Options options;
  // drop_tombstones = true: the merge includes the oldest table, so the delete
  // has nothing left to shadow and can be garbage-collected.
  ASSERT_TRUE(lsm::CompactTables({newest, older}, Path("out.sst"), options, true).ok());

  std::shared_ptr<SSTableReader> out;
  ASSERT_TRUE(SSTableReader::Open(Path("out.sst"), &out).ok());
  std::string v;
  EXPECT_EQ(Get(out, "k", &v), SSTableReader::GetResult::kNotPresent);  // fully gone
}

TEST_F(CompactionTest, KeepsTombstoneWhenOlderTablesRemain) {
  auto newest = MakeTable("0.sst", {Tomb("k")});
  auto older = MakeTable("1.sst", {Val("k", "old")});

  const lsm::Options options;
  // drop_tombstones = false: older tables outside this compaction might hold the
  // key, so the tombstone must survive to keep shadowing them.
  ASSERT_TRUE(lsm::CompactTables({newest, older}, Path("out.sst"), options, false).ok());

  std::shared_ptr<SSTableReader> out;
  ASSERT_TRUE(SSTableReader::Open(Path("out.sst"), &out).ok());
  std::string v;
  EXPECT_EQ(Get(out, "k", &v), SSTableReader::GetResult::kDeleted);  // still shadows
}

TEST_F(CompactionTest, MergesManyKeysInSortedOrder) {
  auto t0 = MakeTable("0.sst", {Val("b", "b0"), Val("d", "d0"), Val("f", "f0")});
  auto t1 = MakeTable("1.sst", {Val("a", "a1"), Val("c", "c1"), Val("e", "e1")});

  const lsm::Options options;
  ASSERT_TRUE(lsm::CompactTables({t0, t1}, Path("out.sst"), options, true).ok());

  std::shared_ptr<SSTableReader> out;
  ASSERT_TRUE(SSTableReader::Open(Path("out.sst"), &out).ok());
  std::string v;
  for (const auto& [key, want] : std::vector<std::pair<std::string, std::string>>{
           {"a", "a1"}, {"b", "b0"}, {"c", "c1"}, {"d", "d0"}, {"e", "e1"}, {"f", "f0"}}) {
    EXPECT_EQ(Get(out, key, &v), SSTableReader::GetResult::kFound) << key;
    EXPECT_EQ(v, want);
  }
}

}  // namespace
