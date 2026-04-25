# Memory Ramp Analysis Summary

- started_utc: 2026-04-25T10:50:00Z
- benchmark: /mnt/external-nvme/BUGBOUNTY/DRIVER_ANALYSIS/NOT_STISLA/scripts/compare_search_auto
- mem_available_bytes: 26931425280
- ram_cap_pct: 40
- cap_bytes: 10772570112
- smoke: 0
- time_metrics: /usr/bin/time -v

## Findings
- Largest pilot case reached `n=128000000`, `queries=200000`, estimated `1.061 GiB`.
- All four cases completed under the 40 percent RAM cap with `0` major page faults.
- Peak RSS grew from `19872 KB` to `1011016 KB`.
- Auto selected `c_openmp` for all four stride-17 cases, matching the large-batch policy.
- Average auto time rose from `2.54 ns/key` at `1M` rows to `5.75 ns/key` at `128M` rows, which suggests memory/TLB pressure is starting to show before page faults appear.

## Average Search Cost
| rows | peak RSS KB | major faults | minor faults | elapsed | auto backend | scalar ns/key | openmp ns/key | fortran ns/key | auto ns/key |
| ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| 1000000 | 19872 | 0 | 5997 | 0:00.05 | `c_openmp` | 7.59 | 5.27 | 15.34 | 2.54 |
| 8000000 | 74560 | 0 | 19670 | 0:00.08 | `c_openmp` | 8.86 | 6.26 | 16.59 | 3.41 |
| 32000000 | 261700 | 0 | 66543 | 0:00.18 | `c_openmp` | 9.02 | 8.12 | 18.04 | 3.46 |
| 128000000 | 1011016 | 0 | 254042 | 0:00.74 | `c_openmp` | 8.46 | 8.12 | 18.01 | 5.75 |

## Cases
- case 1: status=ok, n=1000000, queries=200000, estimated_gib=0.020, benchmark_csv=bench_results/memory_ramp_pilot_20260425T105000Z/1_mem_n1000000_q200000_h100_dense_g1_j0_s17_t16.csv, time_metrics=bench_results/memory_ramp_pilot_20260425T105000Z/1_mem_n1000000_q200000_h100_dense_g1_j0_s17_t16.time.txt
- case 2: status=ok, n=8000000, queries=200000, estimated_gib=0.077, benchmark_csv=bench_results/memory_ramp_pilot_20260425T105000Z/2_mem_n8000000_q200000_h100_dense_g1_j0_s17_t16.csv, time_metrics=bench_results/memory_ramp_pilot_20260425T105000Z/2_mem_n8000000_q200000_h100_dense_g1_j0_s17_t16.time.txt
- case 3: status=ok, n=32000000, queries=200000, estimated_gib=0.274, benchmark_csv=bench_results/memory_ramp_pilot_20260425T105000Z/3_mem_n32000000_q200000_h100_dense_g1_j0_s17_t16.csv, time_metrics=bench_results/memory_ramp_pilot_20260425T105000Z/3_mem_n32000000_q200000_h100_dense_g1_j0_s17_t16.time.txt
- case 4: status=ok, n=128000000, queries=200000, estimated_gib=1.061, benchmark_csv=bench_results/memory_ramp_pilot_20260425T105000Z/4_mem_n128000000_q200000_h100_dense_g1_j0_s17_t16.csv, time_metrics=bench_results/memory_ramp_pilot_20260425T105000Z/4_mem_n128000000_q200000_h100_dense_g1_j0_s17_t16.time.txt
