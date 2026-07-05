#include "skiplist.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

using lsm::SkipList;
using lsm::ValueTag;

TEST(SkipListTest, FindAbsentKeyReturnsNull) {
  SkipList list;
  EXPECT_EQ(list.Find("missing"), nullptr);
  EXPECT_EQ(list.Size(), 0U);
}

TEST(SkipListTest, InsertThenFind) {
  SkipList list;
  list.Insert("b", ValueTag::kValue, "2");
  list.Insert("a", ValueTag::kValue, "1");
  list.Insert("c", ValueTag::kValue, "3");

  const auto* a = list.Find("a");
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(a->value, "1");
  EXPECT_EQ(a->tag, ValueTag::kValue);
  EXPECT_EQ(list.Size(), 3U);
}

TEST(SkipListTest, OverwriteReplacesValueAndReportsPrevBytes) {
  SkipList list;
  const auto r1 = list.Insert("k", ValueTag::kValue, "hello");
  EXPECT_FALSE(r1.overwrote);
  EXPECT_EQ(r1.prev_value_bytes, 0U);

  const auto r2 = list.Insert("k", ValueTag::kValue, "hi");
  EXPECT_TRUE(r2.overwrote);
  EXPECT_EQ(r2.prev_value_bytes, 5U);  // "hello"

  const auto* n = list.Find("k");
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->value, "hi");
  EXPECT_EQ(list.Size(), 1U);
}

TEST(SkipListTest, StoresTombstoneTag) {
  SkipList list;
  list.Insert("d", ValueTag::kTombstone, "");
  const auto* n = list.Find("d");
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->tag, ValueTag::kTombstone);
}

TEST(SkipListTest, IterationIsSorted) {
  SkipList list;
  for (const std::string_view k : {"delta", "alpha", "charlie", "bravo"}) {
    list.Insert(k, ValueTag::kValue, k);
  }
  std::vector<std::string> seen;
  auto it = list.NewIterator();
  for (it.SeekToFirst(); it.Valid(); it.Next()) {
    seen.push_back(it.key());
  }
  EXPECT_EQ(seen, (std::vector<std::string>{"alpha", "bravo", "charlie", "delta"}));
}

TEST(SkipListTest, SeekFindsFirstAtOrAfter) {
  SkipList list;
  list.Insert("a", ValueTag::kValue, "1");
  list.Insert("c", ValueTag::kValue, "3");
  list.Insert("e", ValueTag::kValue, "5");

  auto it = list.NewIterator();
  it.Seek("b");
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), "c");

  it.Seek("e");
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), "e");

  it.Seek("z");
  EXPECT_FALSE(it.Valid());
}

TEST(SkipListTest, EmptyIteratorIsInvalid) {
  SkipList list;
  auto it = list.NewIterator();
  it.SeekToFirst();
  EXPECT_FALSE(it.Valid());
}

TEST(SkipListTest, RandomInsertsMatchOrderedReference) {
  SkipList list;
  std::set<std::string> reference;
  std::mt19937 rng(12345);
  for (int i = 0; i < 2000; ++i) {
    const std::string key = "key" + std::to_string(rng() % 500);
    list.Insert(key, ValueTag::kValue, std::to_string(i));
    reference.insert(key);
  }
  EXPECT_EQ(list.Size(), reference.size());

  std::vector<std::string> seen;
  auto it = list.NewIterator();
  for (it.SeekToFirst(); it.Valid(); it.Next()) {
    seen.push_back(it.key());
  }
  EXPECT_TRUE(std::equal(seen.begin(), seen.end(), reference.begin(), reference.end()));

  for (const std::string& k : reference) {
    EXPECT_NE(list.Find(k), nullptr);
  }
}

}  // namespace
