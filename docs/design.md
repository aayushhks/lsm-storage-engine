# design notes

This document records the design decisions and their tradeoffs, milestone by
milestone. It is written to be defended in an interview without notes: every
choice states what was picked, what it was picked over, and why.

## m1 — skeleton

### scope

A buildable, tested, ci-checked skeleton: the public api, a placeholder engine
behind it, the test rig, sanitizer builds, and the four ci jobs. No durable
storage yet — that starts with the memtable (m2), the wal (m3), and sstable
flush (m4).

### public api

The surface is intentionally small: `Open`, `Close`, `Put`, `Get`, `Delete`.
Keys and values are arbitrary byte strings, carried as `std::string_view` on the
way in (no copy at the boundary, embedded nul bytes are fine) and `std::string`
on the way out.

### errors: a Status type, not exceptions or optional

Every fallible operation returns a small `Status` value (ok / not-found /
corruption / io-error / invalid-argument), leveldb-style.

- **Over exceptions:** a storage engine's failure modes — a short read, a bad
  crc, a missing file — are expected control flow, not exceptional. Explicit
  status values keep the failure paths visible at every call site and avoid
  unwinding across the i/o layer.
- **Over `std::optional` for `Get`:** the kickoff spec sketched `Get -> optional`.
  Once the wal and sstables exist a read can fail for reasons other than
  "absent" (i/o error, corruption), which an optional cannot express. A single
  `Status` across the whole api — with not-found as one code — is uniform and is
  what real engines do. `Get` therefore takes a `std::string*` out-parameter and
  returns `Status`; a missing key is `Status::NotFound`.

### pimpl / abstract interface

`DB` is an abstract base; `DBImpl` is the concrete class in `src/`. The public
header stays free of internal types, and later milestones can add members
(memtable, wal, sstable set) without disturbing the public surface. `DB` is a
polymorphic type held through `unique_ptr`, so it is explicitly non-copyable and
non-movable.

### placeholder store

M1's `DBImpl` holds a `std::map` guarded by a `std::mutex`. This is throwaway:
it exists only so the api contract, the tests, the sanitizers, and ci are
meaningful from day one. M2 replaces it with the hand-written skip-list memtable
behind the same interface. Tombstones are not modeled yet (`Delete` erases);
real tombstone semantics arrive with the memtable.

### build, sanitizers, ci

- C++20, CMake, Ninja. `-Wall -Wextra -Werror` on every first-party target via a
  shared interface library, so tests and benchmarks are held to the engine's
  bar.
- Sanitizers are a whole-build configure choice (`-DLSM_SANITIZE=asan-ubsan` or
  `tsan`), not a per-target flag: asan and tsan cannot share a binary, and the
  flags must reach compile and link of every translation unit. A sanitized run
  is a separate build directory. The tsan slot is wired now and exercised from
  m6 when compaction introduces threads.
- Ci runs four jobs on every push: build-and-test across {gcc, clang} ×
  {Debug, Release}, an asan+ubsan test job, clang-tidy, and clang-format.

## m2 — memtable

### scope

The mutable in-memory write buffer, backed by a hand-written skip list. Put,
Get, and Delete (as tombstones), approximate memory accounting for the flush
decision, and an ordered iterator. This replaces the m1 placeholder `std::map`
inside the engine; the public api is unchanged.

### skip list vs. a balanced tree

Both give ordered storage with O(log n) search. The skip list wins here for two
reasons:

- **Simplicity of a correct implementation.** A skip list inserts by finding the
  predecessor at each level and splicing forward pointers — no rotations, no
  rebalancing, no parent pointers, no red/black invariants to preserve. That is
  far less code to get right and to defend line by line than an AVL or red-black
  tree, and the balance is probabilistic rather than maintained.
- **Concurrency-friendliness.** This is why LevelDB and RocksDB use a skip list
  for the memtable. Inserts touch only the O(log n) forward pointers on the
  search path and never restructure existing nodes, so a single writer can
  publish a new node with a release store while lock-free readers traverse
  safely — no reader ever observes a half-rotated tree. We do not exploit that
  yet (m2 serializes access with a mutex in the engine), but choosing the skip
  list keeps that door open for the concurrency work in m6, and it is the honest
  reason the structure is the industry default here.

The tradeoff we accept: probabilistic (not worst-case) bounds, and slightly
worse cache locality than a compact tree because nodes are individually
allocated. For a memtable that is written sequentially and flushed wholesale,
neither matters much.

Parameters: max height 12, branching factor 4 (a node gains each additional
level with probability 1/4). That averages ~1.33 forward pointers per node and
supports far more entries than a memtable holds before flushing. The level rng
is seeded with a fixed constant so behavior is reproducible in tests.

### tombstone semantics

`Delete(key)` does not erase the entry — it writes a marker node tagged
`kTombstone` with no value bytes, overwriting any live value for that key. This
is essential to an lsm tree: when this memtable is later flushed to an sstable,
the tombstone must be persisted so it can shadow older values for the same key
that still live in older sstables. A `Get` therefore distinguishes three
outcomes — found (live value), deleted (a tombstone is present), and not-present
(the key is absent from this table). At the single-table stage the last two both
read as not-found to the user, but the distinction is what makes the multi-table
read path (m5) correct.

### memory accounting

The memtable maintains an approximate byte count to drive the flush threshold
(m4). A new entry adds key bytes + value bytes + a fixed per-node overhead
estimate; an overwrite adjusts only by the change in value size; a delete
reclaims the replaced value's bytes. The estimate is deliberately approximate —
it only needs to fire a flush near the configured threshold, not to measure heap
usage exactly.

### ownership and safety

Every node is heap-allocated once and owned by a vector of `unique_ptr` in the
skip list, so cleanup is raii with no manual `delete`. Forward pointers are
non-owning raw pointers into those stable heap nodes. The one clang-tidy
suppression in the engine is a scoped `NOLINT` over the fixed-array index math in
`Insert`, where every index is a level provably in `[0, kMaxHeight)`; the check
is left enabled everywhere else.

## m3 — write-ahead log and recovery

### scope

Durability. Every mutation is appended and fsync'd to an append-only log before
it becomes visible in the memtable, and `Open` replays the log to rebuild the
memtable. A torn or corrupt tail record is detected and truncated so recovery
never fails on a bad tail.

### on-disk format

Each record is framed as:

```
[payload length : u32 le][crc32 of payload : u32 le][payload bytes]
```

and the payload is one encoded mutation:

```
[type : u8][key length : u32 le][key bytes][value length : u32 le][value bytes]
```

`type` is 0 for a put and 1 for a delete (a delete carries a zero-length value).
Integers are little-endian and written byte-wise, so the format does not depend
on host endianness. The crc-32 (ieee 802.3, implemented from scratch with a
compile-time table) covers the payload; the length prefix bounds how much to
read. Keys and values are arbitrary byte strings, so lengths — not delimiters —
frame every field.

### write path and ordering

`Put`/`Delete` encode the mutation, `write()` the framed bytes, and `fsync()`
before returning; only then is the memtable updated. This ordering is the whole
point of a wal: if the process dies after the fsync but before (or during) the
memtable update, replay reconstructs the exact state, and if it dies before the
fsync the mutation simply never happened. When the log file is first created its
parent directory is fsync'd too, so the directory entry itself is durable, not
just the file's data.

### recovery and tail corruption

On `Open` the log is read record by record. Recovery stops at the first record
that is incomplete (fewer bytes than its length claims) or whose crc does not
match, and the file is truncated at that boundary so later appends continue from
a clean edge. In an append-only, fsync-per-write log the only place a partial or
corrupt record can appear is the tail — a write that was interrupted by a crash —
so stopping at the first bad record and dropping the remainder is both correct
and the standard rule. Recovery of a good prefix followed by a torn tail yields
exactly the records that were durably committed. A missing log is not an error:
it means nothing was written yet.

### fsync-per-write vs. group commit

We fsync on every write. That makes each successful `Put`/`Delete` individually
durable — the strongest and simplest guarantee — but caps write throughput at
roughly one fsync per operation (one disk flush each), which dominates the write
cost. The standard optimization is **group commit**: batch the records from many
concurrent writers into one `write` + `fsync`, amortizing the flush across the
whole batch and multiplying throughput at the cost of a small latency window
where a just-returned writer's data is not yet flushed (usually resolved by not
acknowledging until the shared fsync completes). Group commit is a listed stretch
goal (2); the profiling milestone (m8) is expected to show fsync as the write-path
hotspot, which is exactly the honest motivation for it.

### where this is headed

The wal grows unbounded until m4, where a memtable flush to an sstable makes the
buffered data durable in a different form and lets the corresponding log be
rotated away. Until then the single `wal.log` is the sole source of durability.

## m4 — sstable flush and the manifest

### scope

When the memtable crosses its size threshold it is written out as an immutable
sorted table (an sstable), a manifest records the live set of tables atomically,
and the wal is rotated away. Reads now consult the memtable and then the
sstables newest-to-oldest. Per-table bloom filters and the read-amplification
analysis are m5; the read path itself lands here because "reopen sees all data"
cannot be tested without it.

### sstable format

```
[data block 0][data block 1] ... [index block][footer]
```

A data block is a run of entries `[keylen u32][key][tag u8][vallen u32][value]`,
written in ascending key order (the memtable iterator provides that order for
free). Blocks are cut at a ~4 KiB boundary. The sparse index holds one entry per
block — `[keylen u32][first key][block offset u64][block size u32]` — so it costs
one index slot per block rather than per key. The fixed 24-byte footer is
`[index offset u64][index size u64][magic u64]`; the reader reads the footer
first, validates the magic, then loads the index.

A point lookup binary-searches the sparse index for the last block whose first
key is `<= target` (the only block that can hold the key, since block *i* owns
`[first_key_i, first_key_{i+1})`), preads just that block, and scans it. The
index lives in memory; data blocks are read on demand with `pread`, which is
positioned and stateless — no shared file offset — so a reader is safe to share
across threads when concurrency arrives in m6.

### the manifest and why atomic rename matters

The manifest is the source of truth for which sstables are live: a small file
holding the next file number to allocate and the live table numbers, newest
first. It is rewritten by `WriteFileAtomic` — write a sibling `.tmp`, fsync it,
`rename()` it over the manifest, then fsync the directory. `rename` is atomic on
posix: a reader (including recovery after a crash) sees either the entire old
manifest or the entire new one, never a spliced mix. That is the whole reason a
checksum is unnecessary here — a torn manifest is simply unrepresentable. Writing
the manifest in place, by contrast, could leave a half-updated table list that
points at nothing or omits a real table.

### flush ordering and the crash-recovery rule

A flush proceeds in a fixed order, and the order *is* the correctness argument:

1. build the sstable and `WriteFileSync` it (data + fsync + directory fsync);
2. commit the manifest atomically, now including the new table;
3. only then rotate the wal — close it, delete it, open a fresh empty one;
4. reset the memtable.

The recovery rule on `Open`: **the manifest names the live sstables; anything
else on disk is ignored; then the wal is replayed on top.** Because the wal is
deleted only *after* the manifest commit, every crash point is safe:

- crash after step 1, before step 2 — the manifest does not mention the new
  table, so it is an ignored orphan, and the wal (not yet rotated) still holds
  every mutation; replay rebuilds the memtable with no loss.
- crash after step 2, before step 3 — the manifest names the table and the wal
  still holds the same mutations; replay re-applies them over identical sstable
  data, which is redundant but consistent (a later flush would just rewrite them).

The `OrphanSstableIgnoredAndWalRecovers` test exercises the first case directly:
an sstable with deliberately wrong data is dropped in the directory without a
manifest, and recovery still returns the correct values from the wal. Orphaned
sstables currently leak on disk; reclaiming them is a listed next step.

### read path

`Get` checks the memtable first — a hit or a tombstone there wins outright,
because it is the newest data — then the sstables newest-to-oldest, stopping at
the first hit or tombstone. This is what makes deletes and overwrites correct
across the flush boundary: a newer tombstone or value shadows anything older.
The cost is read amplification — a miss may touch every table — which is exactly
what the m5 bloom filters exist to cut down, and what m5 measures.

### tradeoff: whole-table-in-memory build

The builder assembles the entire table in a `std::string` before writing it.
That caps transient memory at roughly the flush threshold (a few MiB by default)
and keeps the builder trivial to unit-test as a pure function of its inputs. A
streaming builder that writes blocks as it goes would remove that transient, and
is a reasonable later change; at the current scope the simplicity is worth more.

## m5 — read path and bloom filters

### scope

The multi-table read path (memtable, then sstables newest-to-oldest) already
exists from m4. m5 adds a per-sstable bloom filter, built at flush and stored in
the table, so a lookup can skip a table that definitely lacks the key without
reading any of its blocks. The bloom filter and its hashing are implemented from
scratch.

### bloom filter design

A bloom filter is a bit array of *m* bits with *k* hash functions. To insert a
key, set the *k* bits it hashes to; to test a key, check those *k* bits — if any
is clear the key is definitely absent, and if all are set the key is *probably*
present. There are no false negatives (a stored key never reads back absent),
only false positives, at a tunable rate.

- **Hashing.** One 64-bit hash per key (fnv-1a, hand-written), split into two
  32-bit halves `h1` and `h2`. The *k* probe positions come from double hashing —
  `g_i = h1 + i * h2 (mod m)` — the Kirsch–Mitzenmacher construction. It gives
  *k* well-distributed positions from a single hash rather than computing *k*
  independent hashes, which is both the standard technique and cheaper.
- **Sizing (fpr is the build parameter).** From a target false-positive rate *p*
  the optimal bits-per-key is `m/n = -ln p / (ln 2)^2` and the optimal probe
  count is `k = (m/n) · ln 2`. For the default `p = 0.01` that is ~9.6 bits/key
  and `k = 7`. `k` is clamped to `[1, 30]` and stored in a one-byte header so the
  reader knows how many probes to make; the bit-array length gives *m*.

### bits/key vs. fpr

The achieved false-positive rate is `(1 - e^{-kn/m})^k`. More bits per key means
a lower rate, with diminishing returns:

| bits/key | optimal k | theoretical fpr |
|---------:|----------:|----------------:|
| 5        | 3         | ~9.2%           |
| 10       | 7         | ~1.0%           |
| 15       | 10        | ~0.06%          |
| 20       | 14        | ~0.006%         |

Measured on this implementation at the default `p = 0.01` (n = 10,000 keys,
100,000 negative probes): **9.59 bits/key, k = 7, theoretical 1.00%, empirical
1.08%** — within noise of theory, and the `EmpiricalFalsePositiveRateMatchesTheory`
test asserts the measured rate stays within 2× of the theoretical value with no
false negatives.

### read amplification, before vs. after

Without bloom filters a point lookup that misses must consult every sstable that
could contain the key: binary-search its index and read one data block, so a miss
costs one block read *per table*. With a filter per table, a miss reads a block
only from tables where the filter returns a (true or false) positive — a fraction
`p` of them on average. The `sstable data blocks read` counter makes this
concrete. Across **100 single-key sstables**, looking up a key present in none:

- **without bloom filters: 100 data-block reads** (one per table);
- **with bloom filters (p = 0.01): ~1** (bounded to `< 10` by the test).

That is the whole point of the filter: it converts the miss path from O(number
of tables) block reads into O(number of tables · fpr), which matters more as
tables accumulate before compaction (m6). The cost is the filter's space (~9.6
bits per key at the default rate) and one in-memory bit-probe per table on the
read path.

## m6 — background compaction

### scope

A single background thread performs size-tiered compaction: when enough
similarly sized sstables accumulate it merges them, dropping shadowed values and
garbage-collecting tombstones that are safe to drop. This is the milestone that
introduces real concurrency, so the synchronization is the central design
decision, validated under ThreadSanitizer in ci.

### synchronization: an immutable, reference-counted table-set snapshot

The choice was between a `shared_mutex` (readers share, compaction excludes) and
an immutable snapshot of the file set. We use the **snapshot**.

The live set of tables is a `shared_ptr<const vector<TableHandle>>` — an
immutable value. A single ordinary mutex guards the small mutable state (the
memtable, the current-snapshot pointer, the wal, the file-number counter), and it
is held only for in-memory work. A read takes the lock just long enough to
consult the memtable and copy the snapshot pointer, then releases it and reads
sstables with **no lock held**. Compaction does its heavy work — reading inputs,
the k-way merge, writing the output — entirely outside the lock, and takes the
lock only to publish a new snapshot pointer and rewrite the manifest.

Why this is correct under a concurrent compaction:

- A reader that copied the old snapshot keeps the old `SSTableReader` objects
  alive through the `shared_ptr` refcount, so they are never destroyed mid-read.
- Sstables are immutable and read with `pread` (positioned, no shared file
  offset), so any number of threads — foreground reads and the compaction merge —
  can read the same table at once with no data race.
- When compaction deletes a merged-away input file, a reader still holding it
  open is unaffected: posix keeps the inode until the last descriptor closes, and
  the snapshot keeps that descriptor open. The file just disappears from future
  `Open`s (the manifest no longer names it).

The result is that compaction never blocks a read and never invalidates one, and
the only lock a read contends on is held for a couple of in-memory operations.
This is why it beats a `shared_mutex`, under which a read would hold a shared
lock across its sstable i/o and a compaction's exclusive swap would have to wait
for every in-flight reader. Flushes are still synchronous under the lock (a known
serialization point; an immutable-memtable async flush is the natural next step).

### size-tiered vs. leveled compaction

Both bound the number of sstables a read may consult, but they trade the two
amplifications differently:

- **Write amplification** — how many times a given byte is rewritten over its
  life. Size-tiered rewrites a byte roughly once per tier it passes through, so
  ~O(log_F N) times; leveled rewrites more, because merging a table into the next
  level rewrites the overlapping portion of that whole level — typically ~O(F ·
  levels), a larger constant. **Size-tiered wins on write amplification.**
- **Space amplification** — extra disk beyond the live data. Size-tiered can hold
  several tables of the same tier containing overwritten/deleted versions of the
  same keys before they merge, so it can transiently use ~2× or more. Leveled
  keeps at most one table per key-range per level, so space amplification is low
  (~1.1×). **Leveled wins on space amplification.**
- **Read amplification** — both consult O(number of tables/levels); the bloom
  filters (m5) cut the miss cost either way.

Size-tiered is the right scope here: it is dramatically simpler to implement
correctly (merge a group of whole tables; no per-level key-range bookkeeping),
its lower write amplification suits a write-heavy log-structured store, and the
transient space cost is acceptable for a single-node engine. Leveled's win is
space, which matters most at scale with tight disk budgets — out of scope for
this project and listed as a next step.

### the policy and tombstone gc

Tables are kept newest-first. A table's *tier* is `floor(log_F(size))` with the
fanout `F` = `compaction_min_merge` (default 4). The picker scans for the first
contiguous run of same-tier tables of length ≥ `min_merge` and merges that whole
run into one table (which, being ~`F`× larger, lands in the next tier). Selecting
a **contiguous** run is what keeps precedence correct: the merged output takes the
run's position in the age order, so no table outside the run can sit between the
inputs in age and be wrongly shadowed.

The k-way merge emits, per key, only the newest version across the inputs —
dropping shadowed values for free. A tombstone is dropped only when the
compaction includes the oldest table in the database (`drop_tombstones`): if any
older table outside the merge could still hold the key, the tombstone must
survive to keep shadowing it. Dropping it prematurely would resurrect deleted
data — the classic tombstone-resurrection bug, which the
`KeepsTombstoneWhenOlderTablesRemain` test guards against.

### crash safety

Compaction reuses the m4 rule: write the merged sstable durably, then commit the
manifest atomically (the commit point), then delete the input files. A crash
before the manifest commit leaves the inputs live and the output an ignored
orphan; a crash after it leaves the manifest naming the output, and the stale
inputs become ignored orphans. Either way the on-disk state is consistent.

## m7 — benchmark harness

### scope

A `bench` cli that drives the engine through reproducible workloads and reports
throughput and tail latency, writing raw results to json. A python script renders
the json into the README's table and charts. All numbers come from a single run
on stated hardware, and that run's json is committed — no cherry-picking.

### workloads and method

Five workloads, all through the public api:

- **fill-sequential** — put keys in ascending order (sorted inserts).
- **fill-random** — put the same keys in shuffled order.
- **read-random-uniform** — get keys drawn uniformly from a pre-filled set.
- **read-random-zipfian** — get keys drawn from a zipfian distribution (skew
  0.99), so a small hot set dominates — the realistic skewed-access case.
- **mixed-50-50** — an even split of gets and puts over the pre-filled set.

Keys are the decimal index zero-padded to a fixed width (so fill-sequential is
genuinely sorted); values are fixed-size filler. Everything is seeded, so a run
reproduces. Each operation is timed individually with `steady_clock`; latencies
are collected, sorted, and reported as p50/p95/p99, and throughput is the op
count over the wall-clock span of the measured loop. The zipfian sampler uses a
precomputed cumulative distribution with a binary-search draw. The read and mixed
workloads share one populated database so the (expensive, fsync-bound) fill runs
only once.

### what the numbers say

The committed run is in `bench/results/results.json`, rendered into the README.
The shape of the result is the important part and it is honest about the design:

- **Reads are fast** — several hundred thousand ops/sec (~680k–815k on the
  committed run) at roughly single-microsecond p50, because a hit is served from
  the in-memory memtable or from an sstable via one `pread`, with the bloom
  filter skipping tables that cannot hold the key.
- **Writes are slow** — roughly 1.4k ops/sec, because every write does an
  `fsync` before returning (m3's fsync-per-write durability). The ~650µs p50
  write latency is essentially one disk flush. This is the dominant write-path
  cost and exactly what the m8 profiling pass examines; group commit (stretch
  goal) is the known remedy, and its absence is why the number is what it is.
- **Mixed throughput** sits near the write path's, because the fsync on the write
  half dominates the cheap reads.

Reporting the fsync-bound write number rather than hiding it is the point: the
benchmark exists to find the real bottleneck, not to flatter the engine.

## m8 — profiling pass

### tooling (and an honest caveat)

The reference platform for profiling is `perf record` / `perf stat` with
flamegraphs. This project's ci container has **no `perf`** (no binary,
`perf_event_paranoid=2`, and a host kernel the distro's `linux-tools` do not
match), so profiling here uses **valgrind/callgrind** — deterministic,
instruction-level cpu profiling that needs no kernel perf access — visualized as
gprof2dot call-graphs (`bench/profiling/read_before.png`, `read_after.png`). The
numbers below are callgrind instruction counts plus wall-clock benchmark runs.
The equivalent perf recipe, for a perf-capable host, is recorded in
`bench/profiling/README.md` so the same pass reproduces as flamegraphs there.

### the write path: fsync, off-cpu

Profiling the write path with a cpu profiler is the wrong instrument, and that
itself is the finding: a write's cost is the `fsync` (m3), which blocks *off*
cpu waiting on the disk. A cpu profile shows only the small on-cpu remainder
(wal framing, crc, the memtable insert). The write bottleneck is durability
latency, not cpu work, and the fix is group commit (stretch goal) — not a
code hotspot to chase. So the profiling effort went to the read path, which is
cpu-bound.

### the read path: a 42% memset that pread immediately overwrites

Callgrind on the sstable-backed read path found one dominant hotspot:
**`__memset_avx2_unaligned_erms` at 42%** of read-loop instructions. It came from
`SSTableReader::Get`: each lookup allocated a fresh `std::string` for the data
block and `resize()`d it to the block size, which **zero-fills ~4 KiB** — bytes
that the very next `pread` overwrites. So two out of every five instructions on
the read path were zeroing a buffer that was about to be clobbered, plus ~6% more
in the per-lookup `malloc`/`free`.

### the fix

A per-thread reused block buffer plus a read that skips the zero-fill:

- `Get` now reads into a `thread_local std::string`, reused across lookups, so
  there is no per-lookup allocation. `thread_local` keeps it correct under the
  concurrent read path (each thread has its own; tsan-clean).
- A new `PreadInto` reads `length` bytes into an already-sized buffer without the
  resize zero-fill (`pread` provides the bytes). The buffer only grows — and only
  then zero-fills — when a block larger than any seen so far appears, which after
  warmup effectively never happens. The old `PreadExact` (resize + read) stays
  for the compaction scan path, which is not latency-critical.

### measured gain

Same machine, controlled before/after:

- **Instructions (callgrind, deterministic): read loop 394.5M → 205.7M, a 48%
  reduction.** The memset disappears from the profile entirely; the after-profile
  is dominated by genuine work — key `memcmp`, block `ParseEntry`, and bloom
  probing.
- **Throughput (wall clock, read-random-uniform, sstable-backed): median ~896k →
  ~1.07M ops/sec, ~+19%**, and markedly more consistent — the before run swung
  733k–998k, the after held 1.03M–1.07M, because the eliminated allocation and
  zeroing were also the main source of run-to-run jitter. p99 read latency
  improved ~1.7µs → ~1.3µs.

The wall-clock gain (~19%) is smaller than the instruction drop (48%) because
memset is memory-bandwidth bound and partly overlaps other work, and because the
surviving read cost — comparisons, parsing, hashing — is real. That gap is itself
the honest read: removing 48% of instructions bought ~19% of wall time, not 48%.
