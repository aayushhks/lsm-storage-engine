// Benchmark harness for the lsm engine. Runs reproducible workloads through the
// public api and reports ops/sec and p50/p95/p99/p99.9 latencies, writing the
// results (and the machine specs) to json. Each workload runs several trials and
// the median is reported. See docs/design.md (m7).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "benchmark.h"

namespace {

using lsm::bench::BenchConfig;

// Parses "--name=value" style flags. Unknown flags are ignored.
bool MatchFlag(const std::string& arg, const std::string& name, std::string* value) {
  const std::string prefix = "--" + name + "=";
  if (arg.rfind(prefix, 0) == 0) {
    *value = arg.substr(prefix.size());
    return true;
  }
  return false;
}

void PrintUsage() {
  std::fprintf(stderr,
               "usage: lsm_bench [--workload=NAME] [--num_ops=N] [--num_keys=N]\n"
               "                 [--value_size=N] [--key_size=N] [--seed=N] [--trials=N]\n"
               "                 [--flush_threshold=BYTES] [--db_root=DIR] [--out=FILE]\n"
               "workloads: fill-sequential fill-random read-random-uniform\n"
               "           read-random-zipfian mixed  (default: full suite)\n");
}

void PrintTable(const std::vector<lsm::bench::RunResult>& results) {
  std::printf("\n%-22s %10s %12s %9s %9s %9s %10s %10s\n", "workload", "ops", "ops/sec", "p50(us)",
              "p95(us)", "p99(us)", "p99.9(us)", "max(us)");
  for (const lsm::bench::RunResult& r : results) {
    std::printf("%-22s %10llu %12.0f %9.2f %9.2f %9.2f %10.2f %10.2f\n", r.workload.c_str(),
                static_cast<unsigned long long>(r.num_ops), r.ops_per_sec, r.p50_us, r.p95_us,
                r.p99_us, r.p999_us, r.max_us);
  }
  std::printf("\nops/sec is the median of n trials; spread across trials:\n");
  for (const lsm::bench::RunResult& r : results) {
    std::printf("  %-22s n=%u  min %.0f  max %.0f\n", r.workload.c_str(), r.trials,
                r.ops_per_sec_min, r.ops_per_sec_max);
  }
}

}  // namespace

int main(int argc, char** argv) {
  BenchConfig config;
  std::string workload;
  std::string out_path;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    std::string value;
    if (MatchFlag(arg, "workload", &value)) {
      workload = value;
    } else if (MatchFlag(arg, "num_ops", &value)) {
      config.num_ops = std::strtoull(value.c_str(), nullptr, 10);
    } else if (MatchFlag(arg, "num_keys", &value)) {
      config.num_keys = std::strtoull(value.c_str(), nullptr, 10);
    } else if (MatchFlag(arg, "value_size", &value)) {
      config.value_size = std::strtoull(value.c_str(), nullptr, 10);
    } else if (MatchFlag(arg, "key_size", &value)) {
      config.key_size = std::strtoull(value.c_str(), nullptr, 10);
    } else if (MatchFlag(arg, "seed", &value)) {
      config.seed = std::strtoull(value.c_str(), nullptr, 10);
    } else if (MatchFlag(arg, "trials", &value)) {
      config.trials = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
    } else if (MatchFlag(arg, "flush_threshold", &value)) {
      config.flush_threshold_bytes = std::strtoull(value.c_str(), nullptr, 10);
    } else if (MatchFlag(arg, "db_root", &value)) {
      config.db_root = value;
    } else if (MatchFlag(arg, "out", &value)) {
      out_path = value;
    } else {
      PrintUsage();
      return 1;
    }
  }

  const std::vector<lsm::bench::RunResult> results =
      workload.empty() ? lsm::bench::RunSuite(config)
                       : lsm::bench::RunNamedWorkload(workload, config);
  if (results.empty()) {
    std::fprintf(stderr, "no results (unknown workload '%s')\n", workload.c_str());
    PrintUsage();
    return 1;
  }

  PrintTable(results);
  if (!out_path.empty()) {
    lsm::bench::WriteResultsJson(out_path, config, results);
    std::fprintf(stderr, "\nwrote %s\n", out_path.c_str());
  }
  return 0;
}
