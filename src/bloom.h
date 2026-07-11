#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lsm {

// 64-bit hash of a key, implemented from scratch (fnv-1a). The two 32-bit halves
// feed the double-hashing scheme, so one real hash yields k probe positions.
std::uint64_t BloomHash(std::string_view key);

// Builds a serialized bloom filter over the given key hashes, sized for the
// target false-positive rate. Layout: [k : u8][bit array bytes]. An empty input
// still produces a valid (tiny) filter.
std::string BuildBloomFilter(const std::vector<std::uint64_t>& key_hashes, double target_fpr);

// Read-only view over a serialized filter. A default-constructed or malformed
// filter answers MayContain conservatively (always true), so a missing filter
// never turns into a false miss.
class BloomFilter {
 public:
  BloomFilter() = default;
  explicit BloomFilter(std::string serialized);

  // True if key may be present (subject to the false-positive rate); false means
  // the key is definitely absent.
  bool MayContain(std::string_view key) const;

  std::size_t num_bits() const { return num_bits_; }
  int num_probes() const { return num_probes_; }

 private:
  std::string data_;  // [k : u8][bit array]
  std::size_t num_bits_ = 0;
  int num_probes_ = 0;
};

}  // namespace lsm
