# NOT_STISLA Performance Roadmap

Current state:
- Scalar C is the best path for small batches.
- Auto-selected C/OpenMP is the best path from roughly 32K queries upward on this host.
- Fortran is installed, builds, and passes correctness tests.
- The Fortran merge/walk path is now auto-routed for one dense sorted-query profile: 8,192 queries, 100 percent hits, dense data, stride 1.
- Telemetry exact and range lookups now use the maintained timestamp index when input remains sorted.
- The memory ramp runner exists and has a bounded pilot through 128M rows with zero major page faults.

## 1. Keep The Current Backend Policy

The auto selector should stay conservative. Its default policy remains scalar versus C/OpenMP, with a narrow Fortran exception for sorted contiguous query streams.

Near-term work:
- Keep the 16K static threshold as the conservative default for general workloads.
- Keep private C-side query-shape detection for the Fortran exception: nondecreasing keys, in-range sample hits, small average key step, and 4K-16K query count.
- Keep cache decisions keyed by query shape so a sorted stream cannot poison a general workload bucket.
- Preserve direct scalar APIs for small batch callers, because batch result-writing overhead is visible below 8K queries.

## 2. Narrowly Expand The Fortran Backend

The current Fortran backend now detects nondecreasing query keys and can use a merge/walk path. This is useful for dense sorted streams, but not for general mixed or strided query batches.

Better Fortran candidates:
- C-side query-shape detection is now the adoption path: sorted, contiguous-ish, nonwrapping, and sampled in-array hits.
- Keep explicit benchmark profiles for sorted contiguous query streams so Fortran wins are tracked separately from general workloads.
- Avoid broad auto-routing to Fortran unless the profile matches the narrow win case.
- Consider OpenMP chunking only after merge/walk wins more than one profile.

Adoption gate:
- The current Fortran auto-route is an explicit narrow exception, not a general policy change.
- Fortran must win multiple additional real workload buckets by 10 percent or more before the exception expands.

Latest evidence:
- `bench_results/fortran_auto_route_matrix_setup_20260425T105402Z/analysis_summary.md`
- `8192` dense sorted stride-1 routed to `fortran` and averaged `6.12 ns/key` through auto on warm runs.
- `8192` dense stride-17 stayed on `scalar`.
- `32768` dense cases stayed on `c_openmp`.

## 3. Add RAM-Capacity Timing

Goal: measure when the algorithm stops being compute/cache-bound and becomes memory/TLB/page-pressure bound.

Do this with a bounded ramp, not an uncontrolled memory drain:
- Query system memory with `MemAvailable`.
- Run dataset sizes at safe fractions such as 5%, 10%, 25%, 40%, and 60% of available RAM.
- Abort before allocation if the requested data/query buffers would exceed the configured cap.
- Record allocation time, first-touch time, search time, ns/key, dataset GiB, query count, page-fault counts, and RSS.
- Run cold and warm modes separately.
- Keep swap disabled or explicitly report swap activity; swapped results should be treated as a separate stress test, not normal benchmark data.

Useful metrics:
- `dataset_bytes`
- `queries`
- `major_faults`
- `minor_faults`
- `rss_peak_kb`
- `first_touch_ms`
- `search_ns_per_key`
- `effective_read_gib_s`
- `backend`

Recommended first RAM ramp:
- `n=1M`, `8M`, `32M`, `128M`
- `queries=200K`, `1M`, `4M`
- hit rates: `100`, `50`, `0`
- gap profiles: dense and jittered

## 4. Maintain The Memory Benchmark Runner

`scripts/run_memory_ramp.sh` is in place and should remain the RAM-capacity benchmark entrypoint.

Current behavior:
- Takes `NOT_STISLA_RAM_CAP_PCT`, default `40`.
- Takes `NOT_STISLA_MEMORY_SIZES`, default derived from `MemAvailable`.
- Uses `compare_search_auto` for consistency.
- Emits one directory with CSV files plus an `analysis_summary.md`.
- Refuses to run if projected allocation exceeds cap.
- Benchmark CSV rows include setup timing fields: `data_alloc_ms`, `data_init_ms`, `query_alloc_ms`, `query_init_ms`, and `setup_total_ms`.
- Supports `NOT_STISLA_MEMORY_MODES="cold warm"` with one configurable userland warmup pass for warm mode.
- Benchmark CSV rows include `*_effective_gib_s` proxy metrics. These are dataset-size-normalized search rates, not raw DRAM bandwidth.

Latest pilot:
- `bench_results/memory_ramp_query_scale_20260425T110036Z/analysis_summary.md`
- `n=1M`, `8M`, `32M`, `128M`; `queries=1M` and `4M`; dense 100 percent hits; stride `17`; 3 runs; modes `cold warm`.
- `0` major page faults across all cases; peak RSS reached about `1.19 GB`.
- Auto stayed on `c_openmp`; for `8M+` rows, average auto time stabilized around `4.3-4.5 ns/key`.
- Data first-touch/init was about `433-443 ms` at `128M` rows in the `4M` query cases.

Mixed profile evidence:
- `bench_results/memory_ramp_mixed_profiles_20260425T110159Z/analysis_summary.md`
- `n=1M`, `8M`, `32M`, `128M`; `queries=1M`; hit rates `50` and `0`; dense and jittered gaps; modes `cold warm`.
- `0` major page faults across all cases; peak RSS reached about `1.05 GB`.
- Auto stayed on `c_openmp` for all mixed profiles.

Rejected optimization:
- `bench_results/memory_ramp_policy_confirm_20260425T110721Z/analysis_summary.md`
- A sampled out-of-range scalar exception was tested and rejected. It made auto report `scalar`, but auto includes batch fill/result-writing costs, so it was slower than the existing OpenMP auto route.
- Keep large 0-percent-hit batches on the current `c_openmp` policy until an apples-to-apples scalar batch path exists.

Next runner detail:
- Add an apples-to-apples scalar batch backend if we want to retest out-of-range miss routing.
- Run a thread-count sweep for `c_openmp` on `1M` and `4M` query batches.
- Only add true OS cold-cache testing behind an explicit opt-in because dropping caches affects the whole host.

## 5. Decide Next Optimization From Evidence

Expected outcomes:
- If scalar remains stable as data grows, keep optimizing branch and interpolation behavior.
- If OpenMP scales on large memory sets, tune chunking and thread count by dataset size.
- If both flatten at the same memory bandwidth ceiling, focus on layout, huge pages, first-touch policy, and query locality.
- If page faults dominate, add explicit allocation/first-touch reporting before changing search code.
