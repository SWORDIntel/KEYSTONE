# Memory Ramp Analysis Summary

- started_utc: 2026-04-25T10:59:24Z
- benchmark: /mnt/external-nvme/BUGBOUNTY/DRIVER_ANALYSIS/NOT_STISLA/scripts/compare_search_auto
- mem_available_bytes: 30887632896
- ram_cap_pct: 40
- cap_bytes: 12355053158
- smoke: 0
- modes: cold warm
- warmup_runs: 1
- time_metrics: /usr/bin/time -v

## Findings
- Ran `8` bounded cases: `1M`, `8M`, `32M`, and `128M` rows, each in `cold` and `warm` userland modes.
- `cold` means no benchmark warmup pass after setup; `warm` means one unmeasured warmup pass. This does not drop OS caches.
- All cases completed under the 40 percent RAM cap with `0` major page faults.
- Peak RSS reached `1011720 KB` in cold mode and `1011392 KB` in warm mode at `128M` rows.
- Auto selected `c_openmp` for every case.
- CSV rows now include `*_effective_gib_s` proxy columns using `dataset_gib/(ns_per_key*queries)`. This is a scale-normalized proxy, not raw DRAM bandwidth.

## Average Search Cost
| rows | mode | peak RSS KB | major faults | data init ms | setup total ms | auto backend | auto ns/key | auto effective GiB/s |
| ---: | --- | ---: | ---: | ---: | ---: | --- | ---: | ---: |
| 1000000 | cold | 19340 | 0 | 4.509 | 5.879 | `c_openmp` | 2.99 | 12.600 |
| 1000000 | warm | 19580 | 0 | 3.345 | 4.958 | `c_openmp` | 3.37 | 11.148 |
| 8000000 | cold | 74128 | 0 | 30.501 | 32.317 | `c_openmp` | 3.16 | 94.224 |
| 8000000 | warm | 74304 | 0 | 32.389 | 33.830 | `c_openmp` | 3.28 | 90.881 |
| 32000000 | cold | 261580 | 0 | 114.320 | 115.795 | `c_openmp` | 3.70 | 323.323 |
| 32000000 | warm | 261824 | 0 | 117.152 | 118.597 | `c_openmp` | 3.75 | 319.398 |
| 128000000 | cold | 1011720 | 0 | 444.373 | 445.927 | `c_openmp` | 3.28 | 1461.154 |
| 128000000 | warm | 1011392 | 0 | 459.337 | 460.919 | `c_openmp` | 3.90 | 1246.657 |

## Next
- Run the same cold/warm shape across `queries=1M` and `4M` to reduce small-sample timing noise.
- Add hit-rate `50` and `0` plus jittered gaps before changing search code again.

## Cases
- case 1: status=ok, mode=cold, warmup_runs=0, n=1000000, queries=200000, estimated_gib=0.020, benchmark_csv=bench_results/memory_ramp_cold_warm_20260425T105924Z/1_mem_n1000000_q200000_h100_dense_g1_j0_s17_t16_mcold.csv, time_metrics=bench_results/memory_ramp_cold_warm_20260425T105924Z/1_mem_n1000000_q200000_h100_dense_g1_j0_s17_t16_mcold.time.txt
- case 2: status=ok, mode=warm, warmup_runs=1, n=1000000, queries=200000, estimated_gib=0.020, benchmark_csv=bench_results/memory_ramp_cold_warm_20260425T105924Z/2_mem_n1000000_q200000_h100_dense_g1_j0_s17_t16_mwarm.csv, time_metrics=bench_results/memory_ramp_cold_warm_20260425T105924Z/2_mem_n1000000_q200000_h100_dense_g1_j0_s17_t16_mwarm.time.txt
- case 3: status=ok, mode=cold, warmup_runs=0, n=8000000, queries=200000, estimated_gib=0.077, benchmark_csv=bench_results/memory_ramp_cold_warm_20260425T105924Z/3_mem_n8000000_q200000_h100_dense_g1_j0_s17_t16_mcold.csv, time_metrics=bench_results/memory_ramp_cold_warm_20260425T105924Z/3_mem_n8000000_q200000_h100_dense_g1_j0_s17_t16_mcold.time.txt
- case 4: status=ok, mode=warm, warmup_runs=1, n=8000000, queries=200000, estimated_gib=0.077, benchmark_csv=bench_results/memory_ramp_cold_warm_20260425T105924Z/4_mem_n8000000_q200000_h100_dense_g1_j0_s17_t16_mwarm.csv, time_metrics=bench_results/memory_ramp_cold_warm_20260425T105924Z/4_mem_n8000000_q200000_h100_dense_g1_j0_s17_t16_mwarm.time.txt
- case 5: status=ok, mode=cold, warmup_runs=0, n=32000000, queries=200000, estimated_gib=0.274, benchmark_csv=bench_results/memory_ramp_cold_warm_20260425T105924Z/5_mem_n32000000_q200000_h100_dense_g1_j0_s17_t16_mcold.csv, time_metrics=bench_results/memory_ramp_cold_warm_20260425T105924Z/5_mem_n32000000_q200000_h100_dense_g1_j0_s17_t16_mcold.time.txt
- case 6: status=ok, mode=warm, warmup_runs=1, n=32000000, queries=200000, estimated_gib=0.274, benchmark_csv=bench_results/memory_ramp_cold_warm_20260425T105924Z/6_mem_n32000000_q200000_h100_dense_g1_j0_s17_t16_mwarm.csv, time_metrics=bench_results/memory_ramp_cold_warm_20260425T105924Z/6_mem_n32000000_q200000_h100_dense_g1_j0_s17_t16_mwarm.time.txt
- case 7: status=ok, mode=cold, warmup_runs=0, n=128000000, queries=200000, estimated_gib=1.061, benchmark_csv=bench_results/memory_ramp_cold_warm_20260425T105924Z/7_mem_n128000000_q200000_h100_dense_g1_j0_s17_t16_mcold.csv, time_metrics=bench_results/memory_ramp_cold_warm_20260425T105924Z/7_mem_n128000000_q200000_h100_dense_g1_j0_s17_t16_mcold.time.txt
- case 8: status=ok, mode=warm, warmup_runs=1, n=128000000, queries=200000, estimated_gib=1.061, benchmark_csv=bench_results/memory_ramp_cold_warm_20260425T105924Z/8_mem_n128000000_q200000_h100_dense_g1_j0_s17_t16_mwarm.csv, time_metrics=bench_results/memory_ramp_cold_warm_20260425T105924Z/8_mem_n128000000_q200000_h100_dense_g1_j0_s17_t16_mwarm.time.txt
