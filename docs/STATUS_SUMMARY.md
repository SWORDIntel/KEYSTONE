# KEYSTONE Status Summary

Condensed from DYNAMIC_HOT_PATH_PLAN, FORTRAN_BACKEND_PLAN, IMPROVEMENT_PLAN, OPTIMIZATION_SUMMARY, and PERFORMANCE_ROADMAP.

## Done

### Core Search & SIMD
- Scalar C reference path with correctness cross-checks.
- Runtime CPU feature detection (AVX2, AVX-512, AMX, NEON, SVE).
- AVX-512 branchless search using `_mm512_cmp_epi64_mask` (~1.3–1.4x on small arrays).
- AVX2 chunked search path.
- Software prefetching (L1 `_MM_HINT_T0`, L2 `_MM_HINT_T1`) for medium/large arrays.

### Memory
- `keystone_optimize_array_memory()` — transparent huge-page hints via `madvise(MADV_HUGEPAGE)`.
- Huge-page gating by size threshold (>1MB); non-fatal on failure.
- Memory ramp runner (`scripts/run_memory_ramp.sh`) with bounded RAM cap (`MemAvailable` fraction).
- Zero major page faults in pilot up to 128M rows.

### Batch & Auto-Selection
- `keystone_search_batch_auto()` — dynamic scalar vs C OpenMP selector.
- Calibration cache keyed by CPU feature mask, array-size bucket, query-count bucket, thread count.
- Warmup-excluded median/p95 sampling per candidate path.
- Static fallback threshold (~16K queries on AVX2 host); auto selector can override with measured timing.
- p95 exposed in public `keystone_backend_decision_t`.
- Benchmark matrix runner (`scripts/run_perf_matrix.sh`) with configurable size/query/hit-rate/gap/stride sweeps.

### Fortran Backend
- `fortran/keystone_batch.f90` exports `keystone_batch_search_i64`.
- C adapter `keystone_search_batch_fortran` with `int64_t` ABI.
- Opt-in build via `KEYSTONE_ENABLE_FORTRAN=1`.
- Narrow auto-route exception: 8,192 dense sorted stride-1 queries routed to Fortran when faster.

### Testing & Benchmarks
- Correctness tests for scalar, batch, parallel, and Fortran paths.
- Deterministic hit/miss profiles (100/75/50/25/0%).
- Gap profiles: dense, sparse, jittered.
- Stride profiles: sequential, strided, random.
- Thread scaling sweeps (1–32 threads).

## Still To Do

### Calibration & Cache
- Add workload profile fields (hit-rate, gap, stride) to calibration cache keys and decision output.
- Add cache reliability tests: repeated reuse, fallback-policy verification, correctness under cache hit.
- Add explicit cache-entry provenance flag (measured vs static policy).

### Fortran
- Expand benchmark coverage beyond dense all-hit workloads.
- Decide whether Fortran stays explicit-only or becomes a broader selectable backend.
- Require 10%+ win in multiple real workload buckets before expanding the narrow auto-route exception.

### RAM-Capacity & Scaling
- Add bounded RAM-capacity timing: 5%/10%/25%/40%/60% of `MemAvailable` with page-fault and RSS reporting.
- Thread-count sweep for C OpenMP on 1M and 4M query batches.
- Add apples-to-apples scalar batch backend for fair out-of-range miss routing.
- True OS cold-cache testing (behind explicit opt-in because dropping caches affects the whole host).

### Architecture Candidates (deferred)
- **AVX-512**: Split into isolated object (`src/keystone_avx512.c`), compile only with AVX-512 flags, validate on real AVX-512 hardware.
- **AMX**: Only investigate after AVX-512 is measured; requires a real tiled integer search design.

### Profiling & Measurement
- Tune prefetch distance per workload instead of global fixed distance.
- Measure cache/TLB behavior with `perf stat` before accepting changes.
- Add `effective_read_giB/s` and `first_touch_ms` to standard benchmark reporting.
