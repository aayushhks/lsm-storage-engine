# lsm-storage-engine

[![ci](https://github.com/aayushhks/lsm-storage-engine/actions/workflows/ci.yml/badge.svg)](https://github.com/aayushhks/lsm-storage-engine/actions/workflows/ci.yml)

**[Project overview and benchmarks →](https://lsm-storage-engine.vercel.app/)**

A single-node, log-structured merge-tree key-value store written from scratch in
C++20 — a write-ahead log with crash recovery, immutable SSTables with a sparse
index, per-table bloom filters, and size-tiered background compaction. The engine
depends on the standard library only (CRC32 and the bloom hashing are
hand-rolled); it is tested under Address/UB/Thread sanitizers, benchmarked, and
profiled. It is not a RocksDB competitor — it is a from-scratch engine built to
understand and defend every layer, benchmarked honestly on stated hardware.

Every design decision and its tradeoff is written up in
[`docs/design.md`](docs/design.md).

## architecture

```mermaid
flowchart TD
    put["Put / Delete"] --> wal["WAL — crc32-framed, fsync per write"]
    wal --> mem["memtable — hand-written skip list (sorted, tombstones)"]
    mem -->|"crosses flush threshold"| flush["flush to immutable SSTable"]
    flush --> sst["SSTables — data blocks · sparse index · bloom filter · footer"]
    flush --> man["manifest — live table set, updated by atomic rename"]
    compact["background thread — size-tiered k-way merge"] --> sst
    man -.->|"names the live tables"| sst

    get["Get"] --> mem
    mem -.->|"miss"| sst
    sst -.->|"newest to oldest, bloom-gated, first hit wins"| out["value / not-found"]
```

- **Writes** append a CRC32-framed record to the WAL and `fsync` it *before*
  touching the in-memory skip-list memtable, so a crash never loses an
  acknowledged write.
- **Flush**: when the memtable crosses its size threshold it is written out as an
  immutable SSTable (sorted data blocks + a sparse one-entry-per-block index + a
  bloom filter + a fixed footer); the manifest is updated by atomic rename, then
  the WAL is rotated away.
- **Reads** consult the memtable, then the SSTables newest-to-oldest, stopping at
  the first hit or tombstone; each table's bloom filter lets a lookup skip tables
  that cannot hold the key.
- **Compaction** runs on one background thread: when similarly sized tables
  accumulate it k-way-merges them, dropping shadowed values and safe tombstones.

## highlights

- **Durability with crash recovery** — WAL replay on open; a torn or corrupt tail
  record is detected by CRC and truncated, so recovery never fails on a bad tail.
- **Hand-written skip-list memtable** — ordered, tombstone-aware, with memory
  accounting for the flush threshold.
- **From-scratch SSTable format** — sparse index + bloom filter; point lookups are
  a binary search on the index plus one `pread`.
- **From-scratch bloom filter** — bit array + k probes via double hashing;
  false-positive rate is a build parameter (measured 1.08% at the 1% target).
- **Lock-free-read concurrency** — compaction never blocks or breaks a reader
  (immutable, reference-counted table-set snapshot); verified under ThreadSanitizer.
- **Benchmarked and profiled** — reproducible workloads, committed numbers with
  the tail (p99.9) and the run-to-run spread, and a real read-path optimization
  found by profiling: **−51% instructions**, verified by an A/B build.
- **Green CI** — every push builds on gcc + clang (Debug/Release), runs the suite
  under ASan/UBSan and TSan, and checks clang-tidy and clang-format.

## build and test

Requires CMake 3.20+, a C++20 compiler (gcc 13 / clang 18), and Ninja.

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure

# release
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release

# sanitizers
cmake -S . -B build-asan  -G Ninja -DLSM_SANITIZE=asan-ubsan && ctest --test-dir build-asan
cmake -S . -B build-tsan  -G Ninja -DLSM_SANITIZE=tsan       && ctest --test-dir build-tsan
```

## usage

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

Keys and values are arbitrary byte strings (embedded nul bytes are fine). Every
fallible call returns a `Status`; a missing key is `Status::NotFound`.

## benchmarks

Raw JSON is committed at [`bench/results/results.json`](bench/results/results.json)
and the charts are regenerated from it by `bench/plot_results.py`.

**How these were measured** — the methodology matters more than the numbers:

- **Machine**: Intel Xeon @ 2.80 GHz, 4 vCPU, 16 GB RAM, Ubuntu 24.04, ext4 — a
  **KVM guest, not bare metal**.
- **Config**: 50,000 ops per workload, 100,000-key set, 16-byte keys, 100-byte
  values, 4 MiB flush threshold, seed 42. Each workload runs **3 trials and the
  median is reported**; the min–max column shows the spread.
- **Reads are CPU-bound, not disk-bound.** The working set is ~12 MB on a 16 GB
  machine, so after the warm-up pass it is entirely in page cache. These numbers
  measure the lookup path — bloom probe, index binary search, block parse, one
  `pread` — not the storage device.
- Single-threaded client; each op timed individually with `steady_clock`.

| workload | ops/sec (median) | trials min–max | p50 (µs) | p95 (µs) | p99 (µs) | p99.9 (µs) |
|---|---:|---:|---:|---:|---:|---:|
| fill-sequential | 5,311 | 5,217–5,644 | 176.35 | 285.14 | 456.52 | 627.72 |
| fill-random | 5,375 | 5,175–5,426 | 172.35 | 290.71 | 430.14 | 702.70 |
| read-random-uniform | 763,024 | 745,305–768,018 | 1.12 | 1.78 | 2.51 | 30.81 |
| read-random-zipfian | 851,974 | 811,294–869,776 | 0.87 | 1.58 | 2.51 | 28.00 |
| mixed-50-50 | 10,998 | 9,965–11,138 | 31.38 | 227.12 | 353.06 | 495.21 |

![Throughput by workload](bench/results/ops_per_sec.png)
![Latency percentiles by workload](bench/results/latency.png)

Reads run at hundreds of thousands of ops/sec with roughly single-microsecond p50.
Zipfian beats uniform (852k vs 763k) because skew keeps the hot blocks and
skip-list nodes in cache — the result you'd expect if the benchmark is measuring
something real.

Writes are **fsync-bound**, which means their throughput is a property of the
host's storage rather than of the engine: on a different VM instance the same
binary measured 1,556 ops/sec at a 595 µs p50, and here it measures 5,311 ops/sec
at 176 µs. In both cases **p50 ≈ one `fsync`** — that invariant is the real
result, not the absolute number. Group commit (a listed next step) is the remedy.

Two tail effects are worth naming. Reads sit at 2.5 µs through p99 but jump to
**~30 µs at p99.9** (background compaction plus scheduler preemption on a 4-vCPU
guest). The worst single write in a fill run is **~34 ms**, which is a memtable
flush running synchronously under the write lock — precisely the limitation
listed below, with the benchmark putting a number on it.

```sh
./build-release/bench/lsm_bench --trials=3 --out=bench/results/results.json
python3 bench/plot_results.py bench/results/results.json
```

### profiling story

Profiling the read path (valgrind/callgrind — this container has no `perf`; the
perf recipe is in [`bench/profiling/`](bench/profiling/README.md)) found one
dominant hotspot: `SSTableReader::Get` allocated a fresh block buffer per lookup
and `resize()` zero-filled ~4 KiB that `pread` then immediately overwrote —
**44.7% of read-loop instructions spent on a `memset` that was thrown away**, plus
per-lookup `malloc`/`free` traffic.

The fix was a reused `thread_local` block buffer and a `PreadInto` that skips the
zero-fill. It was verified as an **A/B on one machine**: two builds of the same
tree differing only in `src/sstable.cpp`, run through the same harness.

**Instructions in the read loop: 372.6M → 182.1M (−51%).** Callgrind counts
instructions deterministically, so this reproduces exactly on any machine — which
is why it is the headline rather than a wall-clock figure.

| read-loop cost | before | after | change |
|---|---:|---:|---:|
| `__memset_avx2` (the waste) | 166.5M · 44.7% | 1.2M · 0.7% | gone |
| `ParseEntry` (real work) | 41.6M | 41.6M | unchanged |
| `__memcmp_avx2` (real work) | 35.6M | 35.6M | unchanged |
| malloc / free traffic | ~21.5M | ~5.1M | −76% |
| **total** | **372.6M** | **182.1M** | **−51%** |

The two genuine-work rows are *identical in absolute terms* before and after —
only the overhead disappeared, which is what a real optimization looks like as
opposed to a benchmark that quietly stopped doing the work.

In wall-clock terms, five trials each: **620,018 → 710,090 ops/sec median
(+14.5%)**, p50 1.35 µs → 1.14 µs. The trial ranges do overlap (574.7k–713.6k
before, 634.4k–802.3k after), so the honest reading is that the wall-clock gain is
real but noisy on a shared vCPU, and the instruction count is the number worth
quoting.

| before | after |
|---|---|
| ![before](bench/profiling/read_before.png) | ![after](bench/profiling/read_after.png) |

The bright-green `memset` branch on the left is gone on the right; the remaining
read cost is genuine work — key comparison, block parsing, bloom probing.

## design decisions & tradeoffs

The reasoning behind each is in [`docs/design.md`](docs/design.md); in brief:

- **A `Status` type, not exceptions or `optional`.** A storage engine's failures —
  a short read, a bad CRC — are expected control flow; one uniform `Status` across
  the API keeps every failure path visible.
- **A skip list for the memtable, over a balanced tree.** No rotations to get
  right, and inserts touch only the search path — the property that makes
  lock-free single-writer/multi-reader possible and the reason it is the industry
  default.
- **fsync-per-write durability.** The strongest, simplest guarantee; the tradeoff
  is throughput, which the benchmark quantifies and group commit would recover.
- **Atomic manifest via write-temp + rename.** `rename` is atomic on POSIX, so a
  torn manifest is unrepresentable and no checksum is needed; the manifest commit
  is the single durability point for a flush or compaction.
- **Immutable, reference-counted table-set snapshot for reads.** A read copies a
  snapshot pointer under a brief lock then reads lock-free; a concurrent
  compaction swaps in a new snapshot and can even delete the old files without
  breaking an in-flight read (POSIX keeps the inode alive behind the open fd).
  Chosen over a `shared_mutex`, under which reads would hold a lock across their
  I/O.
- **Size-tiered compaction, over leveled.** Lower write amplification
  (~O(log_F N)) and far simpler to implement correctly; it accepts higher
  transient space amplification, which is the right tradeoff at single-node scope.
- **A bloom filter per SSTable.** Converts a miss from one block read per table
  into ~`fpr` block reads per table; the FPR is a build parameter.

## limitations

Honest scope, stated plainly:

- **Single node** — no distribution, replication, or sharding.
- **Single writer** — writes are serialized under a mutex; reads are concurrent.
- **Point lookups only** — no range scans / iterators (a stretch goal not reached).
- **No transactions, MVCC, or snapshot reads.**
- **No compression.**
- **Size-tiered compaction only** — no leveled compaction.
- **Synchronous flush** — a flush runs under the write lock; a large flush briefly
  stalls writers (an immutable-memtable async flush is the fix).
- **Orphaned SSTables leak** — a table written by a compaction/flush that crashed
  before its manifest commit is correctly ignored on restart but not deleted.

## what I'd build next

Understood but deliberately descoped, roughly in priority order:

- **Group commit** — batch the records from many concurrent writers into a single
  `write` + `fsync`, amortizing the flush across the batch; this is the measured
  write-path bottleneck.
- **Range scans** — a merge iterator across the memtable and SSTables with correct
  shadowing; the per-table ordered scan iterator already exists (compaction uses
  it), so this is mostly a k-way merge with tombstone handling exposed as an API.
- **Leveled compaction** — one sorted run per level with non-overlapping key
  ranges, trading write amplification for much lower space amplification when disk
  is tight.
- **Snapshot reads / MVCC** — tag keys with a sequence number so a reader pins a
  consistent version, formalizing the internal immutable-snapshot mechanism into a
  user-visible snapshot.
- **Async flush** — freeze the full memtable and flush it on a background thread so
  writes never stall under the lock during a flush.
- **Orphan reclamation** — delete on-disk tables the manifest does not name, at
  startup.

## how it was built

Built milestone by milestone, each finished green (warnings-as-errors, clang-tidy,
clang-format, all tests under sanitizers) before the next:

`m1` skeleton · `m2` skip-list memtable · `m3` WAL + recovery · `m4` SSTable flush
+ manifest · `m5` read path + bloom filters · `m6` background compaction · `m7`
benchmark harness · `m8` profiling pass · `m9` README + framing.
