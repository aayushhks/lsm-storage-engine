#include "benchmark.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "lsm/db.h"

namespace lsm::bench {
namespace {

using Clock = std::chrono::steady_clock;

std::string MakeKey(std::uint64_t index, std::size_t key_size) {
  std::string digits = std::to_string(index);
  if (digits.size() >= key_size) {
    return digits;
  }
  return std::string(key_size - digits.size(), '0') + digits;
}

double Percentile(std::vector<double>& sorted_us, double fraction) {
  if (sorted_us.empty()) {
    return 0.0;
  }
  const auto rank = static_cast<std::size_t>(fraction * static_cast<double>(sorted_us.size() - 1));
  return sorted_us[rank];
}

RunResult Summarize(const std::string& name, std::vector<double> latencies_us, double seconds) {
  std::sort(latencies_us.begin(), latencies_us.end());
  RunResult result;
  result.workload = name;
  result.num_ops = latencies_us.size();
  result.seconds = seconds;
  result.ops_per_sec = seconds > 0.0 ? static_cast<double>(latencies_us.size()) / seconds : 0.0;
  result.p50_us = Percentile(latencies_us, 0.50);
  result.p95_us = Percentile(latencies_us, 0.95);
  result.p99_us = Percentile(latencies_us, 0.99);
  return result;
}

// Zipfian sampler over [0, n) via a precomputed cumulative distribution.
class Zipfian {
 public:
  Zipfian(std::uint64_t n, double skew, std::uint64_t seed) : rng_(seed) {
    cdf_.resize(n);
    double sum = 0.0;
    for (std::uint64_t i = 0; i < n; ++i) {
      sum += 1.0 / std::pow(static_cast<double>(i + 1), skew);
      cdf_[i] = sum;
    }
    for (double& c : cdf_) {
      c /= sum;
    }
  }

  std::uint64_t Next() {
    const double u = uniform_(rng_);
    const auto it = std::lower_bound(cdf_.begin(), cdf_.end(), u);
    return static_cast<std::uint64_t>(std::distance(cdf_.begin(), it));
  }

 private:
  std::vector<double> cdf_;
  std::mt19937_64 rng_;
  std::uniform_real_distribution<double> uniform_{0.0, 1.0};
};

std::unique_ptr<lsm::DB> OpenFreshDb(const BenchConfig& config, const std::string& name) {
  const std::string dir = config.db_root + "/" + name;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);

  lsm::Options options;
  options.memtable_flush_threshold_bytes = config.flush_threshold_bytes;
  std::unique_ptr<lsm::DB> db;
  const lsm::Status s = lsm::DB::Open(options, dir, &db);
  if (!s.ok()) {
    std::fprintf(stderr, "open failed: %s\n", s.ToString().c_str());
    return nullptr;
  }
  return db;
}

void Populate(lsm::DB* db, const BenchConfig& config) {
  const std::string value(config.value_size, 'v');
  for (std::uint64_t i = 0; i < config.num_keys; ++i) {
    db->Put(MakeKey(i, config.key_size), value);
  }
}

RunResult RunFill(const BenchConfig& config, bool sequential) {
  const std::string name = sequential ? "fill-sequential" : "fill-random";
  auto db = OpenFreshDb(config, name);
  const std::string value(config.value_size, 'v');

  std::vector<std::uint64_t> order(config.num_ops);
  std::iota(order.begin(), order.end(), 0ULL);
  if (!sequential) {
    std::mt19937_64 rng(config.seed);
    std::shuffle(order.begin(), order.end(), rng);
  }

  std::vector<double> latencies_us;
  latencies_us.reserve(config.num_ops);
  const auto start = Clock::now();
  for (const std::uint64_t index : order) {
    const std::string key = MakeKey(index, config.key_size);
    const auto op_start = Clock::now();
    db->Put(key, value);
    const auto op_end = Clock::now();
    latencies_us.push_back(std::chrono::duration<double, std::micro>(op_end - op_start).count());
  }
  const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
  db->Close();
  return Summarize(name, std::move(latencies_us), seconds);
}

RunResult MeasureReads(lsm::DB* db, const BenchConfig& config, bool zipfian) {
  const std::string name = zipfian ? "read-random-zipfian" : "read-random-uniform";
  std::mt19937_64 rng(config.seed);
  std::uniform_int_distribution<std::uint64_t> uniform(0, config.num_keys - 1);
  Zipfian zipf(config.num_keys, config.zipfian_skew, config.seed);

  std::vector<double> latencies_us;
  latencies_us.reserve(config.num_ops);
  std::string value;
  const auto start = Clock::now();
  for (std::uint64_t i = 0; i < config.num_ops; ++i) {
    const std::uint64_t index = zipfian ? zipf.Next() : uniform(rng);
    const std::string key = MakeKey(index, config.key_size);
    const auto op_start = Clock::now();
    db->Get(key, &value);
    const auto op_end = Clock::now();
    latencies_us.push_back(std::chrono::duration<double, std::micro>(op_end - op_start).count());
  }
  const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
  return Summarize(name, std::move(latencies_us), seconds);
}

RunResult MeasureMixed(lsm::DB* db, const BenchConfig& config) {
  std::mt19937_64 rng(config.seed);
  std::uniform_int_distribution<std::uint64_t> key_dist(0, config.num_keys - 1);
  std::uniform_int_distribution<int> coin(0, 1);
  const std::string value(config.value_size, 'w');

  std::vector<double> latencies_us;
  latencies_us.reserve(config.num_ops);
  std::string read_value;
  const auto start = Clock::now();
  for (std::uint64_t i = 0; i < config.num_ops; ++i) {
    const std::string key = MakeKey(key_dist(rng), config.key_size);
    const bool is_read = coin(rng) == 0;
    const auto op_start = Clock::now();
    if (is_read) {
      db->Get(key, &read_value);
    } else {
      db->Put(key, value);
    }
    const auto op_end = Clock::now();
    latencies_us.push_back(std::chrono::duration<double, std::micro>(op_end - op_start).count());
  }
  const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
  return Summarize("mixed-50-50", std::move(latencies_us), seconds);
}

// Standalone read/mixed workloads open and populate their own database.
RunResult RunReads(const BenchConfig& config, bool zipfian) {
  auto db = OpenFreshDb(config, zipfian ? "read-zipfian" : "read-uniform");
  Populate(db.get(), config);
  RunResult r = MeasureReads(db.get(), config, zipfian);
  db->Close();
  return r;
}

RunResult RunMixed(const BenchConfig& config) {
  auto db = OpenFreshDb(config, "mixed");
  Populate(db.get(), config);
  RunResult r = MeasureMixed(db.get(), config);
  db->Close();
  return r;
}

std::string ReadFirstMatch(const std::string& path, const std::string& prefix) {
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind(prefix, 0) != 0) {
      continue;
    }
    // /proc files use "key: value"; os-release uses key="value".
    const auto sep = line.find_first_of(":=");
    std::string value = (sep == std::string::npos) ? line : line.substr(sep + 1);
    const auto first = value.find_first_not_of(" \t");
    if (first == std::string::npos) {
      return "";
    }
    value = value.substr(first);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
      value = value.substr(1, value.size() - 2);
    }
    return value;
  }
  return "";
}

std::size_t CountLines(const std::string& path, const std::string& prefix) {
  std::ifstream in(path);
  std::string line;
  std::size_t count = 0;
  while (std::getline(in, line)) {
    if (line.rfind(prefix, 0) == 0) {
      ++count;
    }
  }
  return count;
}

std::string JsonEscape(const std::string& s) {
  std::string out;
  for (const char c : s) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  return out;
}

}  // namespace

std::vector<RunResult> RunNamedWorkload(const std::string& name, const BenchConfig& config) {
  if (name == "fill-sequential") {
    return {RunFill(config, true)};
  }
  if (name == "fill-random") {
    return {RunFill(config, false)};
  }
  if (name == "read-random-uniform") {
    return {RunReads(config, false)};
  }
  if (name == "read-random-zipfian") {
    return {RunReads(config, true)};
  }
  if (name == "mixed") {
    return {RunMixed(config)};
  }
  return {};
}

std::vector<RunResult> RunSuite(const BenchConfig& config) {
  std::vector<RunResult> results;

  std::fprintf(stderr, "running fill-sequential ...\n");
  results.push_back(RunFill(config, true));
  std::fprintf(stderr, "running fill-random ...\n");
  results.push_back(RunFill(config, false));

  // The read and mixed workloads share one populated database so the expensive
  // fill happens once. Reads do not mutate it, and mixed runs last.
  std::fprintf(stderr, "populating %llu keys for the read workloads ...\n",
               static_cast<unsigned long long>(config.num_keys));
  auto db = OpenFreshDb(config, "reads");
  Populate(db.get(), config);
  std::fprintf(stderr, "running read-random-uniform ...\n");
  results.push_back(MeasureReads(db.get(), config, false));
  std::fprintf(stderr, "running read-random-zipfian ...\n");
  results.push_back(MeasureReads(db.get(), config, true));
  std::fprintf(stderr, "running mixed ...\n");
  results.push_back(MeasureMixed(db.get(), config));
  db->Close();

  return results;
}

void WriteResultsJson(const std::string& path, const BenchConfig& config,
                      const std::vector<RunResult>& results) {
  const std::string cpu = ReadFirstMatch("/proc/cpuinfo", "model name");
  const std::size_t cores = CountLines("/proc/cpuinfo", "processor");
  const std::string mem = ReadFirstMatch("/proc/meminfo", "MemTotal");
  const std::string os = ReadFirstMatch("/etc/os-release", "PRETTY_NAME");

  std::ostringstream json;
  json << "{\n";
  json << "  \"machine\": {\n";
  json << "    \"cpu_model\": \"" << JsonEscape(cpu) << "\",\n";
  json << "    \"cores\": " << cores << ",\n";
  json << "    \"mem_total\": \"" << JsonEscape(mem) << "\",\n";
  json << "    \"os\": \"" << JsonEscape(os) << "\"\n";
  json << "  },\n";
  json << "  \"config\": {\n";
  json << "    \"num_ops\": " << config.num_ops << ",\n";
  json << "    \"num_keys\": " << config.num_keys << ",\n";
  json << "    \"key_size\": " << config.key_size << ",\n";
  json << "    \"value_size\": " << config.value_size << ",\n";
  json << "    \"flush_threshold_bytes\": " << config.flush_threshold_bytes << ",\n";
  json << "    \"zipfian_skew\": " << config.zipfian_skew << ",\n";
  json << "    \"seed\": " << config.seed << "\n";
  json << "  },\n";
  json << "  \"results\": [\n";
  for (std::size_t i = 0; i < results.size(); ++i) {
    const RunResult& r = results[i];
    json << "    {\n";
    json << "      \"workload\": \"" << r.workload << "\",\n";
    json << "      \"num_ops\": " << r.num_ops << ",\n";
    json << "      \"seconds\": " << r.seconds << ",\n";
    json << "      \"ops_per_sec\": " << r.ops_per_sec << ",\n";
    json << "      \"p50_us\": " << r.p50_us << ",\n";
    json << "      \"p95_us\": " << r.p95_us << ",\n";
    json << "      \"p99_us\": " << r.p99_us << "\n";
    json << "    }" << (i + 1 < results.size() ? "," : "") << "\n";
  }
  json << "  ]\n";
  json << "}\n";

  std::ofstream out(path);
  out << json.str();
}

}  // namespace lsm::bench
