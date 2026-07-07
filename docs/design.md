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
