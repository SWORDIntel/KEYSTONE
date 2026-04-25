# Memory Ramp Analysis Summary

- started_utc: 2026-04-25T11:00:36Z
- benchmark: /mnt/external-nvme/BUGBOUNTY/DRIVER_ANALYSIS/NOT_STISLA/scripts/compare_search_auto
- mem_available_bytes: 30849019904
- ram_cap_pct: 40
- cap_bytes: 12339607961
- smoke: 0
- modes: cold warm
- warmup_runs: 1
- time_metrics: /usr/bin/time -v

## Findings
- Ran `16` bounded query-scale cases: `1M`, `8M`, `32M`, and `128M` rows with `1M` and `4M` queries in both `cold` and `warm` modes.
- All cases completed under the 40 percent RAM cap with `0` major page faults.
- Peak RSS reached `1189892 KB` at `128M` rows and `4M` queries in warm mode.
- Auto selected `c_openmp` for every case.
- With larger query counts, auto stabilized around `4.3-4.5 ns/key` for `8M+` rows. The earlier `200K` query pilot was noisier.
- Warm mode improves direct OpenMP measurements, but auto remains close between cold and warm because its selector/cache path is already hot after the first measured run.

## Average Search Cost
| rows | queries | mode | peak RSS KB | auto backend | auto ns/key | auto effective GiB/s | data init ms | setup total ms |
| ---: | ---: | --- | ---: | --- | ---: | ---: | ---: | ---: |
| 1000000 | 1000000 | cold | 56972 | `c_openmp` | 2.97 | 2.510 | 5.313 | 10.669 |
| 1000000 | 1000000 | warm | 57072 | `c_openmp` | 3.11 | 2.393 | 3.681 | 9.736 |
| 8000000 | 1000000 | cold | 111516 | `c_openmp` | 4.51 | 13.258 | 29.693 | 37.109 |
| 8000000 | 1000000 | warm | 111988 | `c_openmp` | 4.30 | 13.863 | 27.512 | 37.833 |
| 32000000 | 1000000 | cold | 298964 | `c_openmp` | 4.46 | 53.449 | 110.634 | 118.134 |
| 32000000 | 1000000 | warm | 299056 | `c_openmp` | 4.49 | 53.155 | 106.517 | 113.740 |
| 128000000 | 1000000 | cold | 1049236 | `c_openmp` | 4.48 | 212.805 | 415.650 | 423.845 |
| 128000000 | 1000000 | warm | 1049000 | `c_openmp` | 4.48 | 212.926 | 419.158 | 426.357 |
| 1000000 | 4000000 | cold | 197724 | `c_openmp` | 3.40 | 0.549 | 3.383 | 18.537 |
| 1000000 | 4000000 | warm | 197544 | `c_openmp` | 3.41 | 0.547 | 3.120 | 22.224 |
| 8000000 | 4000000 | cold | 252292 | `c_openmp` | 4.31 | 3.459 | 28.357 | 55.987 |
| 8000000 | 4000000 | warm | 252136 | `c_openmp` | 4.24 | 3.514 | 28.157 | 56.096 |
| 32000000 | 4000000 | cold | 439760 | `c_openmp` | 4.43 | 13.442 | 109.862 | 138.079 |
| 32000000 | 4000000 | warm | 439788 | `c_openmp` | 4.44 | 13.410 | 106.735 | 134.703 |
| 128000000 | 4000000 | cold | 1189620 | `c_openmp` | 4.48 | 53.171 | 443.321 | 471.650 |
| 128000000 | 4000000 | warm | 1189892 | `c_openmp` | 4.48 | 53.179 | 433.114 | 461.922 |

## Decision
- Keep the current auto policy. Dense stride-17 memory-scale workloads should stay on `c_openmp`.
- Next performance run should add hit-rate `50` and `0` plus jittered gaps; do not tune code from the dense 100 percent hit profile alone.

## Cases
- case 1: status=ok, mode=cold, warmup_runs=0, n=1000000, queries=1000000, estimated_gib=0.066, benchmark_csv=bench_results/memory_ramp_query_scale_20260425T110036Z/1_mem_n1000000_q1000000_h100_dense_g1_j0_s17_t16_mcold.csv, time_metrics=bench_results/memory_ramp_query_scale_20260425T110036Z/1_mem_n1000000_q1000000_h100_dense_g1_j0_s17_t16_mcold.time.txt
- case 2: status=ok, mode=warm, warmup_runs=1, n=1000000, queries=1000000, estimated_gib=0.066, benchmark_csv=bench_results/memory_ramp_query_scale_20260425T110036Z/2_mem_n1000000_q1000000_h100_dense_g1_j0_s17_t16_mwarm.csv, time_metrics=bench_results/memory_ramp_query_scale_20260425T110036Z/2_mem_n1000000_q1000000_h100_dense_g1_j0_s17_t16_mwarm.time.txt
- case 3: status=ok, mode=cold, warmup_runs=0, n=8000000, queries=1000000, estimated_gib=0.123, benchmark_csv=bench_results/memory_ramp_query_scale_20260425T110036Z/3_mem_n8000000_q1000000_h100_dense_g1_j0_s17_t16_mcold.csv, time_metrics=bench_results/memory_ramp_query_scale_20260425T110036Z/3_mem_n8000000_q1000000_h100_dense_g1_j0_s17_t16_mcold.time.txt
- case 4: status=ok, mode=warm, warmup_runs=1, n=8000000, queries=1000000, estimated_gib=0.123, benchmark_csv=bench_results/memory_ramp_query_scale_20260425T110036Z/4_mem_n8000000_q1000000_h100_dense_g1_j0_s17_t16_mwarm.csv, time_metrics=bench_results/memory_ramp_query_scale_20260425T110036Z/4_mem_n8000000_q1000000_h100_dense_g1_j0_s17_t16_mwarm.time.txt
- case 5: status=ok, mode=cold, warmup_runs=0, n=32000000, queries=1000000, estimated_gib=0.320, benchmark_csv=bench_results/memory_ramp_query_scale_20260425T110036Z/5_mem_n32000000_q1000000_h100_dense_g1_j0_s17_t16_mcold.csv, time_metrics=bench_results/memory_ramp_query_scale_20260425T110036Z/5_mem_n32000000_q1000000_h100_dense_g1_j0_s17_t16_mcold.time.txt
- case 6: status=ok, mode=warm, warmup_runs=1, n=32000000, queries=1000000, estimated_gib=0.320, benchmark_csv=bench_results/memory_ramp_query_scale_20260425T110036Z/6_mem_n32000000_q1000000_h100_dense_g1_j0_s17_t16_mwarm.csv, time_metrics=bench_results/memory_ramp_query_scale_20260425T110036Z/6_mem_n32000000_q1000000_h100_dense_g1_j0_s17_t16_mwarm.time.txt
- case 7: status=ok, mode=cold, warmup_runs=0, n=128000000, queries=1000000, estimated_gib=1.106, benchmark_csv=bench_results/memory_ramp_query_scale_20260425T110036Z/7_mem_n128000000_q1000000_h100_dense_g1_j0_s17_t16_mcold.csv, time_metrics=bench_results/memory_ramp_query_scale_20260425T110036Z/7_mem_n128000000_q1000000_h100_dense_g1_j0_s17_t16_mcold.time.txt
- case 8: status=ok, mode=warm, warmup_runs=1, n=128000000, queries=1000000, estimated_gib=1.106, benchmark_csv=bench_results/memory_ramp_query_scale_20260425T110036Z/8_mem_n128000000_q1000000_h100_dense_g1_j0_s17_t16_mwarm.csv, time_metrics=bench_results/memory_ramp_query_scale_20260425T110036Z/8_mem_n128000000_q1000000_h100_dense_g1_j0_s17_t16_mwarm.time.txt
- case 9: status=ok, mode=cold, warmup_runs=0, n=1000000, queries=4000000, estimated_gib=0.238, benchmark_csv=bench_results/memory_ramp_query_scale_20260425T110036Z/9_mem_n1000000_q4000000_h100_dense_g1_j0_s17_t16_mcold.csv, time_metrics=bench_results/memory_ramp_query_scale_20260425T110036Z/9_mem_n1000000_q4000000_h100_dense_g1_j0_s17_t16_mcold.time.txt
- case 10: status=ok, mode=warm, warmup_runs=1, n=1000000, queries=4000000, estimated_gib=0.238, benchmark_csv=bench_results/memory_ramp_query_scale_20260425T110036Z/10_mem_n1000000_q4000000_h100_dense_g1_j0_s17_t16_mwarm.csv, time_metrics=bench_results/memory_ramp_query_scale_20260425T110036Z/10_mem_n1000000_q4000000_h100_dense_g1_j0_s17_t16_mwarm.time.txt
- case 11: status=ok, mode=cold, warmup_runs=0, n=8000000, queries=4000000, estimated_gib=0.295, benchmark_csv=bench_results/memory_ramp_query_scale_20260425T110036Z/11_mem_n8000000_q4000000_h100_dense_g1_j0_s17_t16_mcold.csv, time_metrics=bench_results/memory_ramp_query_scale_20260425T110036Z/11_mem_n8000000_q4000000_h100_dense_g1_j0_s17_t16_mcold.time.txt
- case 12: status=ok, mode=warm, warmup_runs=1, n=8000000, queries=4000000, estimated_gib=0.295, benchmark_csv=bench_results/memory_ramp_query_scale_20260425T110036Z/12_mem_n8000000_q4000000_h100_dense_g1_j0_s17_t16_mwarm.csv, time_metrics=bench_results/memory_ramp_query_scale_20260425T110036Z/12_mem_n8000000_q4000000_h100_dense_g1_j0_s17_t16_mwarm.time.txt
- case 13: status=ok, mode=cold, warmup_runs=0, n=32000000, queries=4000000, estimated_gib=0.492, benchmark_csv=bench_results/memory_ramp_query_scale_20260425T110036Z/13_mem_n32000000_q4000000_h100_dense_g1_j0_s17_t16_mcold.csv, time_metrics=bench_results/memory_ramp_query_scale_20260425T110036Z/13_mem_n32000000_q4000000_h100_dense_g1_j0_s17_t16_mcold.time.txt
- case 14: status=ok, mode=warm, warmup_runs=1, n=32000000, queries=4000000, estimated_gib=0.492, benchmark_csv=bench_results/memory_ramp_query_scale_20260425T110036Z/14_mem_n32000000_q4000000_h100_dense_g1_j0_s17_t16_mwarm.csv, time_metrics=bench_results/memory_ramp_query_scale_20260425T110036Z/14_mem_n32000000_q4000000_h100_dense_g1_j0_s17_t16_mwarm.time.txt
- case 15: status=ok, mode=cold, warmup_runs=0, n=128000000, queries=4000000, estimated_gib=1.279, benchmark_csv=bench_results/memory_ramp_query_scale_20260425T110036Z/15_mem_n128000000_q4000000_h100_dense_g1_j0_s17_t16_mcold.csv, time_metrics=bench_results/memory_ramp_query_scale_20260425T110036Z/15_mem_n128000000_q4000000_h100_dense_g1_j0_s17_t16_mcold.time.txt
- case 16: status=ok, mode=warm, warmup_runs=1, n=128000000, queries=4000000, estimated_gib=1.279, benchmark_csv=bench_results/memory_ramp_query_scale_20260425T110036Z/16_mem_n128000000_q4000000_h100_dense_g1_j0_s17_t16_mwarm.csv, time_metrics=bench_results/memory_ramp_query_scale_20260425T110036Z/16_mem_n128000000_q4000000_h100_dense_g1_j0_s17_t16_mwarm.time.txt
