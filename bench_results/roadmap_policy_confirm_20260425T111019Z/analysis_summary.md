# Memory Ramp Analysis Summary

- started_utc: 2026-04-25T11:10:57Z
- benchmark: /mnt/external-nvme/BUGBOUNTY/DRIVER_ANALYSIS/NOT_STISLA/scripts/compare_search_auto
- mem_available_bytes: 28203335680
- ram_cap_pct: 80
- cap_bytes: 22562668544
- smoke: 0
- modes: cold warm
- warmup_runs: 1
- time_metrics: /usr/bin/time -v

## Cases
- case 1: status=ok, mode=cold, warmup_runs=0, n=128000000, queries=1000000, estimated_gib=1.106, benchmark_csv=bench_results/roadmap_policy_confirm_20260425T111019Z/1_mem_n128000000_q1000000_h0_dense_g1_j0_s17_t16_mcold.csv, time_metrics=bench_results/roadmap_policy_confirm_20260425T111019Z/1_mem_n128000000_q1000000_h0_dense_g1_j0_s17_t16_mcold.time.txt
- case 2: status=ok, mode=warm, warmup_runs=1, n=128000000, queries=1000000, estimated_gib=1.106, benchmark_csv=bench_results/roadmap_policy_confirm_20260425T111019Z/2_mem_n128000000_q1000000_h0_dense_g1_j0_s17_t16_mwarm.csv, time_metrics=bench_results/roadmap_policy_confirm_20260425T111019Z/2_mem_n128000000_q1000000_h0_dense_g1_j0_s17_t16_mwarm.time.txt
- case 3: status=ok, mode=cold, warmup_runs=0, n=128000000, queries=1000000, estimated_gib=1.106, benchmark_csv=bench_results/roadmap_policy_confirm_20260425T111019Z/3_mem_n128000000_q1000000_h0_sparse_g16_j0_s17_t16_mcold.csv, time_metrics=bench_results/roadmap_policy_confirm_20260425T111019Z/3_mem_n128000000_q1000000_h0_sparse_g16_j0_s17_t16_mcold.time.txt
- case 4: status=ok, mode=warm, warmup_runs=1, n=128000000, queries=1000000, estimated_gib=1.106, benchmark_csv=bench_results/roadmap_policy_confirm_20260425T111019Z/4_mem_n128000000_q1000000_h0_sparse_g16_j0_s17_t16_mwarm.csv, time_metrics=bench_results/roadmap_policy_confirm_20260425T111019Z/4_mem_n128000000_q1000000_h0_sparse_g16_j0_s17_t16_mwarm.time.txt
- case 5: status=ok, mode=cold, warmup_runs=0, n=128000000, queries=4000000, estimated_gib=1.279, benchmark_csv=bench_results/roadmap_policy_confirm_20260425T111019Z/5_mem_n128000000_q4000000_h0_dense_g1_j0_s17_t16_mcold.csv, time_metrics=bench_results/roadmap_policy_confirm_20260425T111019Z/5_mem_n128000000_q4000000_h0_dense_g1_j0_s17_t16_mcold.time.txt
- case 6: status=ok, mode=warm, warmup_runs=1, n=128000000, queries=4000000, estimated_gib=1.279, benchmark_csv=bench_results/roadmap_policy_confirm_20260425T111019Z/6_mem_n128000000_q4000000_h0_dense_g1_j0_s17_t16_mwarm.csv, time_metrics=bench_results/roadmap_policy_confirm_20260425T111019Z/6_mem_n128000000_q4000000_h0_dense_g1_j0_s17_t16_mwarm.time.txt
- case 7: status=ok, mode=cold, warmup_runs=0, n=128000000, queries=4000000, estimated_gib=1.279, benchmark_csv=bench_results/roadmap_policy_confirm_20260425T111019Z/7_mem_n128000000_q4000000_h0_sparse_g16_j0_s17_t16_mcold.csv, time_metrics=bench_results/roadmap_policy_confirm_20260425T111019Z/7_mem_n128000000_q4000000_h0_sparse_g16_j0_s17_t16_mcold.time.txt
- case 8: status=ok, mode=warm, warmup_runs=1, n=128000000, queries=4000000, estimated_gib=1.279, benchmark_csv=bench_results/roadmap_policy_confirm_20260425T111019Z/8_mem_n128000000_q4000000_h0_sparse_g16_j0_s17_t16_mwarm.csv, time_metrics=bench_results/roadmap_policy_confirm_20260425T111019Z/8_mem_n128000000_q4000000_h0_sparse_g16_j0_s17_t16_mwarm.time.txt
