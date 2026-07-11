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
- [x] **m8** — profiling pass: found a 42% wasted memset on reads, fixed it, +19% read throughput
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
| fill-sequential | 1,556 | 595.57 | 937.54 | 1227.50 |
| fill-random | 1,533 | 614.08 | 929.50 | 1190.85 |
| read-random-uniform | 738,001 | 1.08 | 2.05 | 3.80 |
| read-random-zipfian | 935,160 | 0.78 | 1.32 | 2.46 |
| mixed-50-50 | 2,897 | 28.61 | 864.14 | 1726.27 |

![Throughput by workload](bench/results/ops_per_sec.png)
![Latency percentiles by workload](bench/results/latency.png)

Reads run at hundreds of thousands of ops/sec with roughly single-microsecond
p50 (memtable or one `pread`, bloom-filtered). Writes are **fsync-bound** at
~1.5k ops/sec — every write flushes to disk before returning, so p50 is
essentially one `fsync`. Group commit (a listed next step) is the known remedy.

### profiling (M8)

Profiling the read path (valgrind/callgrind — this container has no `perf`; see
[`bench/profiling/`](bench/profiling/README.md)) found one dominant hotspot:
`SSTableReader::Get` allocated a fresh block buffer per lookup and `resize()`
zero-filled ~4 KiB that `pread` then immediately overwrote — **42% of read-loop
instructions spent on a `memset` that was thrown away**, plus ~6% of per-lookup
`malloc`/`free`.

The fix: a reused `thread_local` block buffer and a `PreadInto` that skips the
zero-fill. Measured, same machine, before vs. after:

- **instructions: read loop 394.5M → 205.7M (−48%)** — the memset vanishes.
- **throughput (sstable-backed read-random): median ~896k → ~1.07M ops/sec
  (+19%)**, with far less run-to-run jitter and p99 ~1.7µs → ~1.3µs.

| before | after |
|---|---|
| ![before](bench/profiling/read_before.png) | ![after](bench/profiling/read_after.png) |

The bright green `memset` branch on the left is gone on the right. The remaining
read cost is genuine work — key comparison, block parsing, bloom probing.

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
