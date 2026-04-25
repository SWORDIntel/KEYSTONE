# Memory Ramp Analysis Summary

- started_utc: 2026-04-25T11:01:59Z
- benchmark: /mnt/external-nvme/BUGBOUNTY/DRIVER_ANALYSIS/NOT_STISLA/scripts/compare_search_auto
- mem_available_bytes: 30309572608
- ram_cap_pct: 40
- cap_bytes: 12123829043
- smoke: 0
- modes: cold warm
- warmup_runs: 1
- time_metrics: /usr/bin/time -v

## Findings
- Ran `32` mixed-profile cases: `1M`, `8M`, `32M`, and `128M` rows; `1M` queries; hit rates `50` and `0`; dense and jittered gaps; cold and warm modes.
- All cases completed under the 40 percent RAM cap with `0` major page faults.
- Peak RSS reached `1049292 KB`.
- Auto selected `c_openmp` for every case.
- For `50` percent hits, `c_openmp` remains the right large-batch route. Auto measured roughly `3.0-3.9 ns/key`.
- For `0` percent hits, direct scalar is consistently faster because all miss keys are below the data range and return quickly. Auto currently still pays OpenMP overhead.

## Largest Dataset Snapshot
| hit rate | gap | mode | scalar ns/key | openmp ns/key | fortran ns/key | auto ns/key | auto backend |
| ---: | --- | --- | ---: | ---: | ---: | ---: | --- |
| 50 | dense | cold | 8.62 | 5.56 | 15.37 | 3.84 | `c_openmp` |
| 50 | dense | warm | 8.91 | 4.06 | 15.75 | 3.71 | `c_openmp` |
| 50 | jitter | cold | 8.79 | 5.57 | 15.09 | 3.63 | `c_openmp` |
| 50 | jitter | warm | 8.85 | 4.11 | 15.03 | 3.56 | `c_openmp` |
| 0 | dense | cold | 2.20 | 4.10 | 11.79 | 2.69 | `c_openmp` |
| 0 | dense | warm | 2.23 | 2.60 | 11.63 | 3.11 | `c_openmp` |
| 0 | jitter | cold | 2.16 | 4.17 | 11.37 | 2.59 | `c_openmp` |
| 0 | jitter | warm | 2.36 | 2.61 | 11.67 | 2.70 | `c_openmp` |

## Decision
- Keep `c_openmp` for large mixed hit/miss workloads.
- Add a narrow auto-selector exception for batches whose sampled keys are all below `arr[0]` or all above `arr[n-1]`; route those to scalar to avoid OpenMP overhead on trivial out-of-range misses.

## Cases
- case 1: status=ok, mode=cold, warmup_runs=0, n=1000000, queries=1000000, estimated_gib=0.066, benchmark_csv=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/1_mem_n1000000_q1000000_h50_dense_g1_j0_s17_t16_mcold.csv, time_metrics=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/1_mem_n1000000_q1000000_h50_dense_g1_j0_s17_t16_mcold.time.txt
- case 2: status=ok, mode=warm, warmup_runs=1, n=1000000, queries=1000000, estimated_gib=0.066, benchmark_csv=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/2_mem_n1000000_q1000000_h50_dense_g1_j0_s17_t16_mwarm.csv, time_metrics=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/2_mem_n1000000_q1000000_h50_dense_g1_j0_s17_t16_mwarm.time.txt
- case 3: status=ok, mode=cold, warmup_runs=0, n=1000000, queries=1000000, estimated_gib=0.066, benchmark_csv=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/3_mem_n1000000_q1000000_h50_jitter_g8_j8_s17_t16_mcold.csv, time_metrics=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/3_mem_n1000000_q1000000_h50_jitter_g8_j8_s17_t16_mcold.time.txt
- case 4: status=ok, mode=warm, warmup_runs=1, n=1000000, queries=1000000, estimated_gib=0.066, benchmark_csv=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/4_mem_n1000000_q1000000_h50_jitter_g8_j8_s17_t16_mwarm.csv, time_metrics=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/4_mem_n1000000_q1000000_h50_jitter_g8_j8_s17_t16_mwarm.time.txt
- case 5: status=ok, mode=cold, warmup_runs=0, n=1000000, queries=1000000, estimated_gib=0.066, benchmark_csv=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/5_mem_n1000000_q1000000_h0_dense_g1_j0_s17_t16_mcold.csv, time_metrics=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/5_mem_n1000000_q1000000_h0_dense_g1_j0_s17_t16_mcold.time.txt
- case 6: status=ok, mode=warm, warmup_runs=1, n=1000000, queries=1000000, estimated_gib=0.066, benchmark_csv=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/6_mem_n1000000_q1000000_h0_dense_g1_j0_s17_t16_mwarm.csv, time_metrics=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/6_mem_n1000000_q1000000_h0_dense_g1_j0_s17_t16_mwarm.time.txt
- case 7: status=ok, mode=cold, warmup_runs=0, n=1000000, queries=1000000, estimated_gib=0.066, benchmark_csv=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/7_mem_n1000000_q1000000_h0_jitter_g8_j8_s17_t16_mcold.csv, time_metrics=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/7_mem_n1000000_q1000000_h0_jitter_g8_j8_s17_t16_mcold.time.txt
- case 8: status=ok, mode=warm, warmup_runs=1, n=1000000, queries=1000000, estimated_gib=0.066, benchmark_csv=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/8_mem_n1000000_q1000000_h0_jitter_g8_j8_s17_t16_mwarm.csv, time_metrics=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/8_mem_n1000000_q1000000_h0_jitter_g8_j8_s17_t16_mwarm.time.txt
- case 9: status=ok, mode=cold, warmup_runs=0, n=8000000, queries=1000000, estimated_gib=0.123, benchmark_csv=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/9_mem_n8000000_q1000000_h50_dense_g1_j0_s17_t16_mcold.csv, time_metrics=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/9_mem_n8000000_q1000000_h50_dense_g1_j0_s17_t16_mcold.time.txt
- case 10: status=ok, mode=warm, warmup_runs=1, n=8000000, queries=1000000, estimated_gib=0.123, benchmark_csv=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/10_mem_n8000000_q1000000_h50_dense_g1_j0_s17_t16_mwarm.csv, time_metrics=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/10_mem_n8000000_q1000000_h50_dense_g1_j0_s17_t16_mwarm.time.txt
- case 11: status=ok, mode=cold, warmup_runs=0, n=8000000, queries=1000000, estimated_gib=0.123, benchmark_csv=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/11_mem_n8000000_q1000000_h50_jitter_g8_j8_s17_t16_mcold.csv, time_metrics=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/11_mem_n8000000_q1000000_h50_jitter_g8_j8_s17_t16_mcold.time.txt
- case 12: status=ok, mode=warm, warmup_runs=1, n=8000000, queries=1000000, estimated_gib=0.123, benchmark_csv=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/12_mem_n8000000_q1000000_h50_jitter_g8_j8_s17_t16_mwarm.csv, time_metrics=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/12_mem_n8000000_q1000000_h50_jitter_g8_j8_s17_t16_mwarm.time.txt
- case 13: status=ok, mode=cold, warmup_runs=0, n=8000000, queries=1000000, estimated_gib=0.123, benchmark_csv=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/13_mem_n8000000_q1000000_h0_dense_g1_j0_s17_t16_mcold.csv, time_metrics=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/13_mem_n8000000_q1000000_h0_dense_g1_j0_s17_t16_mcold.time.txt
- case 14: status=ok, mode=warm, warmup_runs=1, n=8000000, queries=1000000, estimated_gib=0.123, benchmark_csv=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/14_mem_n8000000_q1000000_h0_dense_g1_j0_s17_t16_mwarm.csv, time_metrics=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/14_mem_n8000000_q1000000_h0_dense_g1_j0_s17_t16_mwarm.time.txt
- case 15: status=ok, mode=cold, warmup_runs=0, n=8000000, queries=1000000, estimated_gib=0.123, benchmark_csv=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/15_mem_n8000000_q1000000_h0_jitter_g8_j8_s17_t16_mcold.csv, time_metrics=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/15_mem_n8000000_q1000000_h0_jitter_g8_j8_s17_t16_mcold.time.txt
- case 16: status=ok, mode=warm, warmup_runs=1, n=8000000, queries=1000000, estimated_gib=0.123, benchmark_csv=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/16_mem_n8000000_q1000000_h0_jitter_g8_j8_s17_t16_mwarm.csv, time_metrics=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/16_mem_n8000000_q1000000_h0_jitter_g8_j8_s17_t16_mwarm.time.txt
- case 17: status=ok, mode=cold, warmup_runs=0, n=32000000, queries=1000000, estimated_gib=0.320, benchmark_csv=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/17_mem_n32000000_q1000000_h50_dense_g1_j0_s17_t16_mcold.csv, time_metrics=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/17_mem_n32000000_q1000000_h50_dense_g1_j0_s17_t16_mcold.time.txt
- case 18: status=ok, mode=warm, warmup_runs=1, n=32000000, queries=1000000, estimated_gib=0.320, benchmark_csv=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/18_mem_n32000000_q1000000_h50_dense_g1_j0_s17_t16_mwarm.csv, time_metrics=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/18_mem_n32000000_q1000000_h50_dense_g1_j0_s17_t16_mwarm.time.txt
- case 19: status=ok, mode=cold, warmup_runs=0, n=32000000, queries=1000000, estimated_gib=0.320, benchmark_csv=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/19_mem_n32000000_q1000000_h50_jitter_g8_j8_s17_t16_mcold.csv, time_metrics=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/19_mem_n32000000_q1000000_h50_jitter_g8_j8_s17_t16_mcold.time.txt
- case 20: status=ok, mode=warm, warmup_runs=1, n=32000000, queries=1000000, estimated_gib=0.320, benchmark_csv=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/20_mem_n32000000_q1000000_h50_jitter_g8_j8_s17_t16_mwarm.csv, time_metrics=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/20_mem_n32000000_q1000000_h50_jitter_g8_j8_s17_t16_mwarm.time.txt
- case 21: status=ok, mode=cold, warmup_runs=0, n=32000000, queries=1000000, estimated_gib=0.320, benchmark_csv=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/21_mem_n32000000_q1000000_h0_dense_g1_j0_s17_t16_mcold.csv, time_metrics=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/21_mem_n32000000_q1000000_h0_dense_g1_j0_s17_t16_mcold.time.txt
- case 22: status=ok, mode=warm, warmup_runs=1, n=32000000, queries=1000000, estimated_gib=0.320, benchmark_csv=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/22_mem_n32000000_q1000000_h0_dense_g1_j0_s17_t16_mwarm.csv, time_metrics=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/22_mem_n32000000_q1000000_h0_dense_g1_j0_s17_t16_mwarm.time.txt
- case 23: status=ok, mode=cold, warmup_runs=0, n=32000000, queries=1000000, estimated_gib=0.320, benchmark_csv=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/23_mem_n32000000_q1000000_h0_jitter_g8_j8_s17_t16_mcold.csv, time_metrics=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/23_mem_n32000000_q1000000_h0_jitter_g8_j8_s17_t16_mcold.time.txt
- case 24: status=ok, mode=warm, warmup_runs=1, n=32000000, queries=1000000, estimated_gib=0.320, benchmark_csv=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/24_mem_n32000000_q1000000_h0_jitter_g8_j8_s17_t16_mwarm.csv, time_metrics=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/24_mem_n32000000_q1000000_h0_jitter_g8_j8_s17_t16_mwarm.time.txt
- case 25: status=ok, mode=cold, warmup_runs=0, n=128000000, queries=1000000, estimated_gib=1.106, benchmark_csv=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/25_mem_n128000000_q1000000_h50_dense_g1_j0_s17_t16_mcold.csv, time_metrics=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/25_mem_n128000000_q1000000_h50_dense_g1_j0_s17_t16_mcold.time.txt
- case 26: status=ok, mode=warm, warmup_runs=1, n=128000000, queries=1000000, estimated_gib=1.106, benchmark_csv=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/26_mem_n128000000_q1000000_h50_dense_g1_j0_s17_t16_mwarm.csv, time_metrics=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/26_mem_n128000000_q1000000_h50_dense_g1_j0_s17_t16_mwarm.time.txt
- case 27: status=ok, mode=cold, warmup_runs=0, n=128000000, queries=1000000, estimated_gib=1.106, benchmark_csv=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/27_mem_n128000000_q1000000_h50_jitter_g8_j8_s17_t16_mcold.csv, time_metrics=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/27_mem_n128000000_q1000000_h50_jitter_g8_j8_s17_t16_mcold.time.txt
- case 28: status=ok, mode=warm, warmup_runs=1, n=128000000, queries=1000000, estimated_gib=1.106, benchmark_csv=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/28_mem_n128000000_q1000000_h50_jitter_g8_j8_s17_t16_mwarm.csv, time_metrics=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/28_mem_n128000000_q1000000_h50_jitter_g8_j8_s17_t16_mwarm.time.txt
- case 29: status=ok, mode=cold, warmup_runs=0, n=128000000, queries=1000000, estimated_gib=1.106, benchmark_csv=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/29_mem_n128000000_q1000000_h0_dense_g1_j0_s17_t16_mcold.csv, time_metrics=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/29_mem_n128000000_q1000000_h0_dense_g1_j0_s17_t16_mcold.time.txt
- case 30: status=ok, mode=warm, warmup_runs=1, n=128000000, queries=1000000, estimated_gib=1.106, benchmark_csv=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/30_mem_n128000000_q1000000_h0_dense_g1_j0_s17_t16_mwarm.csv, time_metrics=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/30_mem_n128000000_q1000000_h0_dense_g1_j0_s17_t16_mwarm.time.txt
- case 31: status=ok, mode=cold, warmup_runs=0, n=128000000, queries=1000000, estimated_gib=1.106, benchmark_csv=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/31_mem_n128000000_q1000000_h0_jitter_g8_j8_s17_t16_mcold.csv, time_metrics=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/31_mem_n128000000_q1000000_h0_jitter_g8_j8_s17_t16_mcold.time.txt
- case 32: status=ok, mode=warm, warmup_runs=1, n=128000000, queries=1000000, estimated_gib=1.106, benchmark_csv=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/32_mem_n128000000_q1000000_h0_jitter_g8_j8_s17_t16_mwarm.csv, time_metrics=bench_results/memory_ramp_mixed_profiles_20260425T110159Z/32_mem_n128000000_q1000000_h0_jitter_g8_j8_s17_t16_mwarm.time.txt
