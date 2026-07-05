#include "memtable.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

namespace {

using lsm::MemTable;
using lsm::ValueTag;

TEST(MemTableTest, PutThenGet) {
  MemTable m;
  m.Put("k", "v");
  std::string value;
  EXPECT_EQ(m.Get("k", &value), MemTable::GetResult::kFound);
  EXPECT_EQ(value, "v");
}

TEST(MemTableTest, GetAbsentIsNotPresent) {
  MemTable m;
  std::string value;
  EXPECT_EQ(m.Get("nope", &value), MemTable::GetResult::kNotPresent);
}

TEST(MemTableTest, OverwriteKeepsLatest) {
  MemTable m;
  m.Put("k", "first");
  m.Put("k", "second");
  std::string value;
  EXPECT_EQ(m.Get("k", &value), MemTable::GetResult::kFound);
  EXPECT_EQ(value, "second");
  EXPECT_EQ(m.NumEntries(), 1U);
}

TEST(MemTableTest, DeleteLeavesTombstoneThatShadows) {
  MemTable m;
  m.Put("k", "v");
  m.Delete("k");
  std::string value;
  // deleted, not merely absent: the distinction matters for shadowing older
  // sstables once flush and the multi-table read path exist.
  EXPECT_EQ(m.Get("k", &value), MemTable::GetResult::kDeleted);
  EXPECT_EQ(m.NumEntries(), 1U);
}

TEST(MemTableTest, DeleteAbsentKeyStillWritesTombstone) {
  MemTable m;
  m.Delete("ghost");
  std::string value;
  EXPECT_EQ(m.Get("ghost", &value), MemTable::GetResult::kDeleted);
  EXPECT_EQ(m.NumEntries(), 1U);
}

TEST(MemTableTest, IteratorYieldsSortedEntriesIncludingTombstones) {
  MemTable m;
  m.Put("c", "3");
  m.Delete("b");
  m.Put("a", "1");

  std::vector<std::string> keys;
  std::vector<ValueTag> tags;
  auto it = m.NewIterator();
  for (it.SeekToFirst(); it.Valid(); it.Next()) {
    keys.push_back(it.key());
    tags.push_back(it.tag());
  }
  EXPECT_EQ(keys, (std::vector<std::string>{"a", "b", "c"}));
  ASSERT_EQ(tags.size(), 3U);
  EXPECT_EQ(tags[0], ValueTag::kValue);
  EXPECT_EQ(tags[1], ValueTag::kTombstone);
  EXPECT_EQ(tags[2], ValueTag::kValue);
}

TEST(MemTableTest, MemoryAccountingGrowsAndShrinks) {
  MemTable m;
  EXPECT_TRUE(m.Empty());
  m.Put("key", "value");
  const std::size_t after_put = m.ApproximateMemoryUsage();
  EXPECT_GT(after_put, 0U);
  EXPECT_FALSE(m.Empty());

  // overwriting with a shorter value reduces the estimate.
  m.Put("key", "v");
  EXPECT_LT(m.ApproximateMemoryUsage(), after_put);

  // overwriting with a longer value raises it back above the original.
  m.Put("key", "a much longer value than the first one");
  EXPECT_GT(m.ApproximateMemoryUsage(), after_put);
}

TEST(MemTableTest, DeleteReclaimsValueBytes) {
  MemTable m;
  m.Put("k", "a reasonably sized value");
  const std::size_t after_put = m.ApproximateMemoryUsage();
  m.Delete("k");
  EXPECT_LT(m.ApproximateMemoryUsage(), after_put);
}

TEST(MemTableTest, HandlesEmbeddedNulBytes) {
  MemTable m;
  const std::string key("a\0b", 3);
  const std::string val("x\0y", 3);
  m.Put(key, val);
  std::string got;
  ASSERT_EQ(m.Get(key, &got), MemTable::GetResult::kFound);
  EXPECT_EQ(got, val);
}

}  // namespace
