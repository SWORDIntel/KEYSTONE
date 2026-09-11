# KEYSTONE Status Summary

Condensed from DYNAMIC_HOT_PATH_PLAN, FORTRAN_BACKEND_PLAN, IMPROVEMENT_PLAN, OPTIMIZATION_SUMMARY, and PERFORMANCE_ROADMAP.

## Done

### Core Search & SIMD
- Scalar C reference path with correctness cross-checks.
- Runtime CPU feature detection (AVX2, AVX-512, AMX, NEON, SVE).
- AVX2 local scan path for small search windows when built for x86 AVX2.
- AVX-512 local scan path is build-gated and should be treated as target-silicon experimental until measured on real AVX-512 hardware.
- Software prefetching (L1 `_MM_HINT_T0`, L2 `_MM_HINT_T1`) for medium/large arrays.

### Memory
- `keystone_optimize_array_memory()` — transparent huge-page hints via `madvise(MADV_HUGEPAGE)`.
- Huge-page gating by size threshold (>1MB); non-fatal on failure.
- Memory ramp runner (`scripts/run_memory_ramp.sh`) with bounded RAM cap (`MemAvailable` fraction).
- Zero major page faults in pilot up to 128M rows.

### Batch & Auto-Selection
- `keystone_search_batch_auto()` — first-use local calibration across viable scalar/C batch, OpenMP, and Fortran candidates.
- Calibration cache keyed by CPU feature mask, array-size bucket, query-count bucket, thread count, hit-rate bucket, gap bucket, and detected stride.
- Static fallback remains for invalid/uncalibrated fallback cases, but normal uncached decisions use measured median/p95 timing.
- Workload profile fields (`hit_rate_pct`, `avg_gap`, `detected_stride`) exposed in public `keystone_backend_decision_t` and Python `BackendDecision`.
- Fallback-policy verification for cache bypass (`KEYSTONE_DISABLE_CALIBRATION_CACHE`) and static fallback paths (`KEYSTONE_FORCE_CALIBRATION_FALLBACK`).
- Richer host/build metadata (hostname, OS, arch, kernel release, compiler name, compiler version) added to benchmark writer and calibration CSV/JSON output.
- p95 field exposed in public `keystone_backend_decision_t` from calibration samples or cached measured values.
- Decision provenance is exposed as fast path, measured, cache, or static fallback.
- Query shape is exposed as general, dense sorted, sparse sorted, strided, or random.
- Cache-hit correctness tests compare result and ordinal arrays across measured and cached selector runs.
- `scripts/compare_search.c` CSV output includes auto backend, decision source, query shape, calibration run count, candidate count, hit-rate, gap, stride, and host/build comments.
- Benchmark matrix runner (`scripts/run_perf_matrix.sh`) with configurable size/query/hit-rate/gap/stride sweeps.

### Fortran Backend
- `fortran/keystone_batch.f90` exports `keystone_batch_search_i64`.
- C adapter `keystone_search_batch_fortran` with `int64_t` ABI.
- Optional native build: enabled explicitly with `KEYSTONE_ENABLE_FORTRAN=1`, or auto-enabled by the Makefile when `gfortran` is present unless `KEYSTONE_ENABLE_FORTRAN=0`.
- Narrow auto-route exception for dense sorted query shapes within the configured query-count window.

### Testing & Benchmarks
- Correctness tests for scalar, batch, parallel API behavior, optional Fortran paths, and archive handling.
- Deterministic hit/miss profiles (100/75/50/25/0%).
- Gap profiles: dense, sparse, jittered.
- Stride profiles: sequential, strided, random.
- Thread scaling sweeps (1–32 threads).

### Vector Similarity Engine
- Standalone vector search engine (`vector_engine/`) for 384-dim (and arbitrary-dim) float32 embeddings with cosine, L2, and dot metrics.
- LSH coarse index with multiprobe search, 4-way unrolled FMA projection, 64-bit quick bloom filter candidate deduplication, and $O(N \log N)$ `qsort` bucket finalization.
- Multi-tier SIMD distance kernels: Scalar, SSE4.2, AVX, AVX2, AVX-512, ARM NEON, Myriad X VPU, and CUDA with graceful runtime fallback.
- Zero-heap query scratchpads for cosine query normalization (up to 1,024 dims) and candidate reranking (up to 512 candidates).
- Compiler-agnostic OpenMP batch search (`keystone_vec_search_batch`) auto-detected during vector engine build.

### Archive Streaming & Ingestion
- Streaming `.tar.zst` reader (`src/keystone_tar_zst.c`) with zero on-disk inflation.
- Persistent sidecar indices (`<archive>.idx.json`) for sub-millisecond archive startup and $O(1)$ negative rejection via compact Bloom filters.
- Dual-threaded producer/consumer ring-buffered decompression (`enable_pipeline`) overlapping I/O with parsing.
- Non-destructive stream rewind (`keystone_tar_zst_rewind()`) enabling out-of-order member queries.
- Multi-archive parallel batch pools (`keystone_tar_zst_batch_*`) with OpenMP parallel candidate evaluation.

### Concurrency & Hash Indexing
- Zero-copy LSD radix sort (`src/dsmil_hash_indexer.c`): replaced 32 full-array `memcpy` operations across 8 passes with double-buffered pointer swapping.
- Reader-writer lock (`pthread_rwlock_t`) on auto-backend calibration cache (`src/keystone.c`), enabling lock-free concurrent lookups.
- QIHSE bridge authentication: auto-delegating credential dispatch to `keystone_qihse_bridge_dispatch_credential_authenticated()` when principal is configured, enforcing QIHSE Invariant 1.

### Trigram Content Indexing (tgrep-style)
- Native C11 trigram index engine (`include/keystone_trigram.h`, `src/keystone_trigram.c`).
- 24-bit hash trigram extraction and inverted posting list intersection for sub-linear text document search.
- Achieves 100x+ search speedups on large text/log corpora by rejecting non-matching candidates before full verification.
- **Unified C Interface**: Integrated directly into `<keystone.h>` for single-include developer access.
- **Case-Insensitive Mode**: `KEYSTONE_TRIGRAM_OPT_CASE_INSENSITIVE` flag with SIMD SSE4.2 character folding and case-insensitive substring verification.
- **Binary Persistence**: `keystone_trigram_index_save()` and `keystone_trigram_index_load()` for zero-rebuild fast loading from disk.
- **Direct `.tar.zst` Streaming Ingestion**: `keystone_tar_zst_index_trigram()` indexes compressed archive text members directly via stream buffers without unpacking to disk.
- **DSMIL Integration**: `dsmil_trigram_index_tar_zst()` provides a unified one-call archive member indexer for log triage.
- **Python SDK Bindings**: Full `TrigramIndex` support with streaming ingestion, case-insensitivity, persistence (`save`/`load`), and candidate iteration.

## Still To Do

The items below are the current engineering backlog. The root README intentionally keeps this detail out of the executive overview; see [TECHNICAL_OVERVIEW.md](TECHNICAL_OVERVIEW.md) for the surrounding architecture.

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
- **AMX**: CPU feature detection exists, but no AMX search backend should be claimed until there is a real tiled integer search design.
- **GPU/NPU Vector Acceleration**: CUDA and Myriad X VPU kernels are implemented in `vector_engine/` with runtime soft-loading; continuing hardware-in-the-loop stress testing on resident silicon.

### Profiling & Measurement
- Tune prefetch distance per workload instead of global fixed distance.
- Measure cache/TLB behavior with `perf stat` before accepting changes.
- Add `effective_read_giB/s` and `first_touch_ms` to standard benchmark reporting.
