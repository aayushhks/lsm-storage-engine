#include "bloom.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using lsm::BloomFilter;
using lsm::BloomHash;
using lsm::BuildBloomFilter;

std::vector<std::uint64_t> HashKeys(const std::string& prefix, int count) {
  std::vector<std::uint64_t> hashes;
  hashes.reserve(count);
  for (int i = 0; i < count; ++i) {
    hashes.push_back(BloomHash(prefix + std::to_string(i)));
  }
  return hashes;
}

TEST(BloomTest, NoFalseNegatives) {
  const int n = 5000;
  BloomFilter filter(BuildBloomFilter(HashKeys("key", n), 0.01));
  for (int i = 0; i < n; ++i) {
    EXPECT_TRUE(filter.MayContain("key" + std::to_string(i))) << "missed key" << i;
  }
}

TEST(BloomTest, EmptyFilterIsConservative) {
  const BloomFilter filter;  // default: no data
  EXPECT_TRUE(filter.MayContain("anything"));
}

TEST(BloomTest, DisjointKeysAreMostlyExcluded) {
  BloomFilter filter(BuildBloomFilter(HashKeys("in", 1000), 0.01));
  int excluded = 0;
  for (int i = 0; i < 1000; ++i) {
    if (!filter.MayContain("out" + std::to_string(i))) {
      ++excluded;
    }
  }
  EXPECT_GT(excluded, 950);  // at ~1% fpr the vast majority are correctly excluded
}

TEST(BloomTest, EmpiricalFalsePositiveRateMatchesTheory) {
  const double target = 0.01;
  const int n = 10000;
  BloomFilter filter(BuildBloomFilter(HashKeys("key", n), target));

  const int trials = 20000;
  int false_positives = 0;
  for (int i = 0; i < trials; ++i) {
    if (filter.MayContain("absent" + std::to_string(i))) {
      ++false_positives;
    }
  }
  const double empirical = static_cast<double>(false_positives) / trials;

  // theoretical fpr for the actual parameters chosen: (1 - e^{-k n / m})^k.
  const double k = filter.num_probes();
  const double m = static_cast<double>(filter.num_bits());
  const double theory = std::pow(1.0 - std::exp(-k * n / m), k);

  EXPECT_LT(empirical, theory * 2.0) << "empirical=" << empirical << " theory=" << theory;
  EXPECT_GT(empirical, theory * 0.5) << "empirical=" << empirical << " theory=" << theory;
  EXPECT_LT(empirical, target * 2.5) << "empirical=" << empirical << " target=" << target;
}

TEST(BloomTest, EmptyKeySetStillValid) {
  const std::vector<std::uint64_t> none;
  BloomFilter filter(BuildBloomFilter(none, 0.01));
  EXPECT_FALSE(filter.MayContain("anything"));  // an empty set contains nothing
}

}  // namespace
