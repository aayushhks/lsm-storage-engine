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
