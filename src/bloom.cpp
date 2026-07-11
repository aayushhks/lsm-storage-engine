#include "bloom.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <utility>

namespace lsm {
namespace {

// fnv-1a 64-bit constants.
constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

constexpr double kLn2 = std::numbers::ln2;

// Optimal probe count k = (m/n) * ln2, clamped to a sane, byte-storable range.
int OptimalProbes(double bits_per_key) {
  const auto k = static_cast<int>(std::lround(bits_per_key * kLn2));
  return std::clamp(k, 1, 30);
}

// Sets or tests the k probe positions for one key hash. Positions come from
// double hashing: g_i = h1 + i*h2 (mod num_bits), the two halves of one hash.
struct Probe {
  std::uint32_t h1;
  std::uint32_t h2;
};

Probe MakeProbe(std::uint64_t hash) {
  auto h2 = static_cast<std::uint32_t>(hash >> 32U);
  if (h2 == 0) {
    h2 = 1;  // avoid a degenerate stride that probes the same bit k times
  }
  return Probe{static_cast<std::uint32_t>(hash), h2};
}

std::size_t BitPosition(const Probe& p, int i, std::size_t num_bits) {
  const std::size_t stride = static_cast<std::size_t>(p.h2) * static_cast<std::size_t>(i);
  return (static_cast<std::size_t>(p.h1) + stride) % num_bits;
}

}  // namespace

std::uint64_t BloomHash(std::string_view key) {
  std::uint64_t hash = kFnvOffset;
  for (const char c : key) {
    hash ^= static_cast<unsigned char>(c);
    hash *= kFnvPrime;
  }
  return hash;
}

std::string BuildBloomFilter(const std::vector<std::uint64_t>& key_hashes, double target_fpr) {
  const std::size_t n = key_hashes.size();

  // bits per key for the target fpr: m/n = -ln(p) / (ln2)^2.
  double bits_per_key = -std::log(target_fpr) / (kLn2 * kLn2);
  bits_per_key = std::max(bits_per_key, 1.0);
  const int num_probes = OptimalProbes(bits_per_key);

  const auto keys = static_cast<double>(std::max<std::size_t>(n, 1));
  auto num_bits = static_cast<std::size_t>(keys * bits_per_key);
  num_bits = std::max<std::size_t>(num_bits, 8);
  const std::size_t num_bytes = (num_bits + 7) / 8;
  num_bits = num_bytes * 8;

  std::string data;
  data.push_back(static_cast<char>(static_cast<unsigned char>(num_probes)));
  data.append(num_bytes, '\0');

  for (const std::uint64_t hash : key_hashes) {
    const Probe probe = MakeProbe(hash);
    for (int i = 0; i < num_probes; ++i) {
      const std::size_t bit = BitPosition(probe, i, num_bits);
      const std::size_t byte_index = 1 + (bit / 8);  // +1 skips the k header byte
      const auto mask = static_cast<unsigned char>(1U << (bit % 8));
      data[byte_index] = static_cast<char>(static_cast<unsigned char>(data[byte_index]) | mask);
    }
  }
  return data;
}

BloomFilter::BloomFilter(std::string serialized) : data_(std::move(serialized)) {
  if (data_.size() > 1) {
    num_probes_ = static_cast<unsigned char>(data_[0]);
    num_bits_ = (data_.size() - 1) * 8;
  }
  if (num_probes_ <= 0 || num_bits_ == 0) {
    num_probes_ = 0;
    num_bits_ = 0;
  }
}

bool BloomFilter::MayContain(std::string_view key) const {
  if (num_bits_ == 0 || num_probes_ == 0) {
    return true;  // no usable filter: never skip on our account
  }
  const Probe probe = MakeProbe(BloomHash(key));
  for (int i = 0; i < num_probes_; ++i) {
    const std::size_t bit = BitPosition(probe, i, num_bits_);
    const std::size_t byte_index = 1 + (bit / 8);
    const auto mask = static_cast<unsigned char>(1U << (bit % 8));
    if ((static_cast<unsigned char>(data_[byte_index]) & mask) == 0) {
      return false;  // a clear bit proves the key was never inserted
    }
  }
  return true;
}

}  // namespace lsm
