Hardware: Intel(R) Xeon(R) Processor @ 2.10GHz · 4 cores · Ubuntu 24.04.4 LTS, 16461176 kB.
Config: 50000 ops/workload, 100000 keys, 100-byte values, 16-byte keys, flush at 4194304 bytes.

| workload | ops/sec | p50 (us) | p95 (us) | p99 (us) |
|---|---:|---:|---:|---:|
| fill-sequential | 1,556 | 595.57 | 937.54 | 1227.50 |
| fill-random | 1,533 | 614.08 | 929.50 | 1190.85 |
| read-random-uniform | 738,001 | 1.08 | 2.05 | 3.80 |
| read-random-zipfian | 935,160 | 0.78 | 1.32 | 2.46 |
| mixed-50-50 | 2,897 | 28.61 | 864.14 | 1726.27 |
