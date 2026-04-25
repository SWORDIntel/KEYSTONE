# Memory Ramp Analysis Summary

- started_utc: 2026-04-25T10:53:03Z
- benchmark: /mnt/external-nvme/BUGBOUNTY/DRIVER_ANALYSIS/NOT_STISLA/scripts/compare_search_auto
- mem_available_bytes: 31437107200
- ram_cap_pct: 40
- cap_bytes: 12574842880
- smoke: 0
- time_metrics: /usr/bin/time -v

## Findings
- Largest pilot case reached `n=128000000`, `queries=200000`, estimated `1.061 GiB`.
- All four cases completed under the 40 percent RAM cap with `0` major page faults.
- Peak RSS grew from `19584 KB` to `1011888 KB`.
- Setup timing is now present in each benchmark CSV as `data_alloc_ms`, `data_init_ms`, `query_alloc_ms`, `query_init_ms`, and `setup_total_ms`.
- Data first-touch/init rose from `5.518 ms` at `1M` rows to `425.073 ms` at `128M` rows.
- Auto selected `c_openmp` for all four stride-17 cases, matching the large-batch policy.

## Average Search Cost And Setup
| rows | peak RSS KB | major faults | minor faults | data init ms | setup total ms | auto backend | scalar ns/key | openmp ns/key | fortran ns/key | auto ns/key |
| ---: | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| 1000000 | 19584 | 0 | 5995 | 5.518 | 6.600 | `c_openmp` | 7.59 | 6.23 | 16.33 | 3.18 |
| 8000000 | 74304 | 0 | 19666 | 28.915 | 30.314 | `c_openmp` | 9.38 | 5.67 | 18.66 | 3.33 |
| 32000000 | 261672 | 0 | 66541 | 107.514 | 109.026 | `c_openmp` | 10.37 | 6.60 | 16.69 | 3.58 |
| 128000000 | 1011888 | 0 | 254044 | 425.073 | 426.566 | `c_openmp` | 9.82 | 7.31 | 18.70 | 3.60 |

## Cases
- case 1: status=ok, n=1000000, queries=200000, estimated_gib=0.020, benchmark_csv=bench_results/memory_ramp_pilot_setup_20260425T105303Z/1_mem_n1000000_q200000_h100_dense_g1_j0_s17_t16.csv, time_metrics=bench_results/memory_ramp_pilot_setup_20260425T105303Z/1_mem_n1000000_q200000_h100_dense_g1_j0_s17_t16.time.txt
- case 2: status=ok, n=8000000, queries=200000, estimated_gib=0.077, benchmark_csv=bench_results/memory_ramp_pilot_setup_20260425T105303Z/2_mem_n8000000_q200000_h100_dense_g1_j0_s17_t16.csv, time_metrics=bench_results/memory_ramp_pilot_setup_20260425T105303Z/2_mem_n8000000_q200000_h100_dense_g1_j0_s17_t16.time.txt
- case 3: status=ok, n=32000000, queries=200000, estimated_gib=0.274, benchmark_csv=bench_results/memory_ramp_pilot_setup_20260425T105303Z/3_mem_n32000000_q200000_h100_dense_g1_j0_s17_t16.csv, time_metrics=bench_results/memory_ramp_pilot_setup_20260425T105303Z/3_mem_n32000000_q200000_h100_dense_g1_j0_s17_t16.time.txt
- case 4: status=ok, n=128000000, queries=200000, estimated_gib=1.061, benchmark_csv=bench_results/memory_ramp_pilot_setup_20260425T105303Z/4_mem_n128000000_q200000_h100_dense_g1_j0_s17_t16.csv, time_metrics=bench_results/memory_ramp_pilot_setup_20260425T105303Z/4_mem_n128000000_q200000_h100_dense_g1_j0_s17_t16.time.txt
