# profiling (m8)

The read-path optimization in m8 was found by profiling. This container has no
`perf` (no binary; `perf_event_paranoid=2`; the host kernel does not match the
distro `linux-tools`), so the committed profiles use **valgrind/callgrind**,
which is deterministic and needs no kernel perf access.

## what is here

- `read_before.png` — call-graph of the sstable-backed read path before the fix.
  The bright green branch `SSTableReader::Get → PreadExact → __memset_avx2` at
  ~42% is the hotspot: a per-lookup block buffer that `resize()` zero-fills right
  before `pread` overwrites it.
- `read_after.png` — the same path after switching to a reused `thread_local`
  buffer and a `PreadInto` that skips the zero-fill. The memset branch is gone;
  the read loop is 48% fewer instructions and dominated by real work (`memcmp`,
  `ParseEntry`, bloom probing).

## reproduce the callgrind profile

```sh
cmake -S . -B build-prof -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-prof --target lsm_bench

valgrind --tool=callgrind --collect-atstart=no --toggle-collect='*MeasureReads*' \
  --callgrind-out-file=read.callgrind --cache-sim=no --branch-sim=no \
  ./build-prof/bench/lsm_bench --workload=read-random-uniform \
  --num_keys=8000 --num_ops=40000 --flush_threshold=65536 --db_root=/tmp/profdb

callgrind_annotate --inclusive=no read.callgrind | head -30           # hotspot table
python3 -m gprof2dot -f callgrind -n 2 -e 1 read.callgrind | dot -Tpng -o read.png
```

## reproduce as a perf flamegraph (on a perf-capable host)

```sh
# needs perf and Brendan Gregg's FlameGraph scripts on PATH
perf record -F 997 -g --call-graph=dwarf -- \
  ./build-prof/bench/lsm_bench --workload=read-random-uniform \
  --num_keys=100000 --num_ops=1000000 --db_root=/tmp/profdb
perf script | stackcollapse-perf.pl | flamegraph.pl > read.svg

perf stat -d -- ./build-prof/bench/lsm_bench --workload=fill-random --num_ops=50000
```

`perf stat` on the write path confirms it is fsync-bound (off-cpu i/o wait, not
cpu), which is why the m8 optimization targeted the cpu-bound read path.
