# lsm-storage-engine

[![ci](https://github.com/aayushhks/lsm-storage-engine/actions/workflows/ci.yml/badge.svg)](https://github.com/aayushhks/lsm-storage-engine/actions/workflows/ci.yml)

A single-node, log-structured merge-tree key-value store written from scratch in
C++20 — write-ahead log, SSTables, bloom filters, and background compaction —
built for depth and benchmarked honestly on stated hardware. Standard library
only for the engine itself.

This is a work in progress, built milestone by milestone. The full README —
architecture diagram, benchmark table, profiling story, and design tradeoffs —
arrives with the later milestones. Design decisions are recorded as they are
made in [`docs/design.md`](docs/design.md).

## status

- [x] **m1** — skeleton: public api, build, tests, sanitizers, ci
- [x] **m2** — skip-list memtable: ordered store, tombstones, memory accounting
- [x] **m3** — write-ahead log + recovery: crc32 framing, fsync-per-write, tail truncation
- [x] **m4** — sstable flush + manifest: sparse-index tables, atomic manifest, wal rotation
- [x] **m5** — read path + bloom filters: from-scratch bloom, ~1% fpr, cuts miss read-amp
- [x] **m6** — background compaction: size-tiered, k-way merge, immutable-snapshot reads, tsan-clean
- [x] **m7** — benchmark harness: reproducible workloads, ops/sec + p50/p95/p99, json + charts
- [ ] m8 — profiling pass
- [ ] m9 — readme + framing

## build and test

Requires CMake 3.20+, a C++20 compiler, and Ninja.

```sh
# configure and build (debug by default)
cmake -S . -B build -G Ninja
cmake --build build

# run the tests
ctest --test-dir build --output-on-failure

# release build
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
```

Run the test suite under AddressSanitizer + UndefinedBehaviorSanitizer:

```sh
cmake -S . -B build-asan -G Ninja -DLSM_SANITIZE=asan-ubsan
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

## api

```cpp
#include "lsm/db.h"

lsm::Options options;
std::unique_ptr<lsm::DB> db;
lsm::Status s = lsm::DB::Open(options, "/path/to/db", &db);

db->Put("key", "value");

std::string value;
if (db->Get("key", &value).ok()) {
  // found
}

db->Delete("key");
db->Close();  // also happens on destruction
```

## benchmarks

Numbers from a single run of the `bench` harness on the stated hardware — no
cherry-picking; the raw JSON is committed at [`bench/results/results.json`](bench/results/results.json)
and the charts are regenerated from it by `bench/plot_results.py`.

Hardware: **Intel Xeon @ 2.10 GHz, 4 cores, 16 GB RAM, Ubuntu 24.04 (ext4)**.
Config: 50,000 ops per workload, 100,000-key read set, 16-byte keys, 100-byte
values, 4 MiB flush threshold.

| workload | ops/sec | p50 (µs) | p95 (µs) | p99 (µs) |
|---|---:|---:|---:|---:|
| fill-sequential | 1,440 | 647.20 | 1018.59 | 1367.30 |
| fill-random | 1,422 | 649.32 | 1055.64 | 1427.08 |
| read-random-uniform | 683,215 | 1.18 | 2.16 | 3.90 |
| read-random-zipfian | 814,684 | 0.90 | 1.58 | 2.70 |
| mixed-50-50 | 2,930 | 32.89 | 867.83 | 1105.70 |

![Throughput by workload](bench/results/ops_per_sec.png)
![Latency percentiles by workload](bench/results/latency.png)

Reads run at hundreds of thousands of ops/sec with roughly single-microsecond
p50 (memtable or one `pread`, bloom-filtered). Writes are **fsync-bound** at
~1.4k ops/sec — every write flushes to disk before returning, so p50 is
essentially one `fsync`. That is the honest bottleneck the profiling pass (M8)
targets; group commit is the known remedy.

To reproduce:

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
./build-release/bench/lsm_bench --out=bench/results/results.json
python3 bench/plot_results.py bench/results/results.json
```

## limitations

Honest scope, stated up front: single node, single writer, point lookups. No
transactions, MVCC, compression, or leveled compaction. This does not compete
with RocksDB or LevelDB; it is a from-scratch engine built to understand and
defend every layer. Limitations and planned next steps are tracked as the
project progresses.
