#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lsm::bench {

// Knobs for a benchmark run. Keys and values are fixed-size byte strings; the
// key is the decimal index zero-padded to key_size, so fill-sequential produces
// sorted keys.
struct BenchConfig {
  std::uint64_t num_ops = 50000;    // measured operations per workload
  std::uint64_t num_keys = 100000;  // distinct keys the read set is filled with
  std::size_t key_size = 16;
  std::size_t value_size = 100;
  std::uint64_t seed = 42;
  double zipfian_skew = 0.99;
  std::size_t flush_threshold_bytes = 4ULL << 20;  // 4 MiB
  std::uint32_t trials = 3;  // repeats per workload; the median trial is reported
  std::string db_root = "/tmp/lsm_bench";
};

// Latency and throughput for one workload. When a workload is repeated these
// are the median trial's numbers, and ops_per_sec_min/max bound the spread
// across trials so run-to-run noise is visible rather than hidden.
struct RunResult {
  std::string workload;
  std::uint64_t num_ops = 0;
  double seconds = 0.0;
  double ops_per_sec = 0.0;
  double p50_us = 0.0;
  double p95_us = 0.0;
  double p99_us = 0.0;
  double p999_us = 0.0;
  double max_us = 0.0;
  std::uint32_t trials = 1;
  double ops_per_sec_min = 0.0;
  double ops_per_sec_max = 0.0;
};

// Runs the full standard suite (fill-sequential, fill-random, read-random
// uniform, read-random zipfian, mixed) and returns one result each. Progress is
// written to stderr.
std::vector<RunResult> RunSuite(const BenchConfig& config);

// Runs a single named workload, or returns an empty vector for an unknown name.
std::vector<RunResult> RunNamedWorkload(const std::string& name, const BenchConfig& config);

// Writes machine specs, config, and results to path as JSON.
void WriteResultsJson(const std::string& path, const BenchConfig& config,
                      const std::vector<RunResult>& results);

}  // namespace lsm::bench
