# Memory Ramp Analysis Summary

- started_utc: 2026-04-25T11:07:21Z
- benchmark: /mnt/external-nvme/BUGBOUNTY/DRIVER_ANALYSIS/NOT_STISLA/scripts/compare_search_auto
- mem_available_bytes: 29957545984
- ram_cap_pct: 40
- cap_bytes: 11983018393
- smoke: 0
- modes: cold warm
- warmup_runs: 1
- time_metrics: /usr/bin/time -v

## Findings
- Confirmed final policy after rejecting the out-of-range scalar exception.
- Auto selected `c_openmp` for the large 0-percent-hit dense workload.
- Cold mode: scalar `2.27 ns/key`, direct OpenMP `4.41 ns/key`, auto `2.61 ns/key`.
- Warm mode: scalar `2.32 ns/key`, direct OpenMP `2.60 ns/key`, auto `2.72 ns/key`.
- There were `0` major page faults; peak RSS was `1049244 KB`.

## Decision
- Keep the existing large-batch OpenMP auto route. Direct scalar numbers are not comparable to auto because auto includes batch fill/result-writing costs.
- Do not add an out-of-range scalar exception unless the benchmark harness gains an apples-to-apples scalar batch path.

## Cases
- case 1: status=ok, mode=cold, warmup_runs=0, n=128000000, queries=1000000, estimated_gib=1.106, benchmark_csv=bench_results/memory_ramp_policy_confirm_20260425T110721Z/1_mem_n128000000_q1000000_h0_dense_g1_j0_s17_t16_mcold.csv, time_metrics=bench_results/memory_ramp_policy_confirm_20260425T110721Z/1_mem_n128000000_q1000000_h0_dense_g1_j0_s17_t16_mcold.time.txt
- case 2: status=ok, mode=warm, warmup_runs=1, n=128000000, queries=1000000, estimated_gib=1.106, benchmark_csv=bench_results/memory_ramp_policy_confirm_20260425T110721Z/2_mem_n128000000_q1000000_h0_dense_g1_j0_s17_t16_mwarm.csv, time_metrics=bench_results/memory_ramp_policy_confirm_20260425T110721Z/2_mem_n128000000_q1000000_h0_dense_g1_j0_s17_t16_mwarm.time.txt
