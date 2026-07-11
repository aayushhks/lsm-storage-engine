Hardware: Intel(R) Xeon(R) Processor @ 2.10GHz · 4 cores · Ubuntu 24.04.4 LTS, 16461176 kB.
Config: 50000 ops/workload, 100000 keys, 100-byte values, 16-byte keys, flush at 4194304 bytes.

| workload | ops/sec | p50 (us) | p95 (us) | p99 (us) |
|---|---:|---:|---:|---:|
| fill-sequential | 1,440 | 647.20 | 1018.59 | 1367.30 |
| fill-random | 1,422 | 649.32 | 1055.64 | 1427.08 |
| read-random-uniform | 683,215 | 1.18 | 2.16 | 3.90 |
| read-random-zipfian | 814,684 | 0.90 | 1.58 | 2.70 |
| mixed-50-50 | 2,930 | 32.89 | 867.83 | 1105.70 |
