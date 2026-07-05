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
- [ ] m2 — skip-list memtable
- [ ] m3 — write-ahead log + recovery
- [ ] m4 — sstable flush + manifest
- [ ] m5 — read path + bloom filters
- [ ] m6 — background compaction
- [ ] m7 — benchmark harness
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

## limitations

Honest scope, stated up front: single node, single writer, point lookups. No
transactions, MVCC, compression, or leveled compaction. This does not compete
with RocksDB or LevelDB; it is a from-scratch engine built to understand and
defend every layer. Limitations and planned next steps are tracked as the
project progresses.
