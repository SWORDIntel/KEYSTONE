# Memory Ramp Analysis Summary

- started_utc: 2026-04-25T10:59:03Z
- benchmark: /mnt/external-nvme/BUGBOUNTY/DRIVER_ANALYSIS/NOT_STISLA/scripts/compare_search_auto
- mem_available_bytes: 30900424704
- ram_cap_pct: 40
- cap_bytes: 12360169881
- smoke: 1
- modes: cold warm
- warmup_runs: 1
- time_metrics: /usr/bin/time -v

## Cases
- case 1: status=ok, mode=cold, warmup_runs=0, n=4096, queries=1024, estimated_gib=0.000, benchmark_csv=bench_results/memory_ramp_modes_smoke_20260425T105903Z/1_mem_n4096_q1024_h100_dense_g1_j0_s17_t16_mcold.csv, time_metrics=bench_results/memory_ramp_modes_smoke_20260425T105903Z/1_mem_n4096_q1024_h100_dense_g1_j0_s17_t16_mcold.time.txt
- case 2: status=ok, mode=warm, warmup_runs=1, n=4096, queries=1024, estimated_gib=0.000, benchmark_csv=bench_results/memory_ramp_modes_smoke_20260425T105903Z/2_mem_n4096_q1024_h100_dense_g1_j0_s17_t16_mwarm.csv, time_metrics=bench_results/memory_ramp_modes_smoke_20260425T105903Z/2_mem_n4096_q1024_h100_dense_g1_j0_s17_t16_mwarm.time.txt
