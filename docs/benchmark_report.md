## KEYSTONE Benchmark Comparison

This report is an example benchmark snapshot. Treat the numbers as host/build-specific, not universal guarantees. Re-run the benchmark suite on the target machine with the exact build flags and workload profile attached before making performance claims.

| Metric | Binary Search | KEYSTONE Search | KEYSTONE Batch Parallel |
| --- | --- | --- | --- |
| Mean latency (ns/op) | 44.97 | 7.98 | 6.48 |
| Speedup vs binary | 1.0× | 5.6× | 6.9× |
| Speedup vs classical | 0.18× | 1.0× | 1.23× |
| Best run (ns/op) | 43.27 | 7.14 | 3.87 |
| Worst run (ns/op) | 49.04 | 8.70 | 13.20 |
| Dataset | Sorted `int64_t` (1 000 000 elements) | Same | Same |
| Query workload | 200 000 random in-array values | Same | Same |

In this captured run, per-core classical `keystone_search` outperformed the binary-search baseline, and the batch/parallel path improved further. Reproduce on the target host before treating the speedups as project-wide claims.
