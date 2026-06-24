# Changelog

All notable changes to the KEYSTONE search engine are documented in this file.

## [1.1.0] - Upcoming

### API Changes
- **Auto-Backend Router**: Added `keystone_search_batch_auto` for intelligent, shape-aware batch query routing across Scalar, SIMD, and parallel boundaries.
- **Decision Provenance**: Exposed `keystone_backend_decision_t` and `keystone_get_last_backend_decision()` enabling full inspection of the router's hardware choices, calibration runs, and detected array shapes.
- **Archive Streamer API**: Introduced `keystone_tar_zst_t` and related methods for direct search over compressed archives (`.tar.zst`).
- **Telemetry Processing**: Introduced `dsmil_telemetry_processor.h` combining archive parsing with KEYSTONE's search.
- **Stable Metadata Documentation**: Promoted `keystone_backend_decision_t` fields to stable Doxygen comments.

### Benchmark Changes
- **CSV & JSON Reporting**: Integrated `-csv` and `-json` outputs in `dsmil_benchmark` and `performance_proof` for easier dashboard consumption.
- **Matrix Runner**: Created `run_perf_matrix.sh` with `perf stat` hardware counter tracking (cache misses, branch misses).
- **Archive Benchmarking**: Large archive extraction (`test_data.tar.zst`) now leverages `dsmil_search_batch_tar_zst` in batch workloads for extremely high-throughput telemetry simulation.
- **Host Capability Logs**: The benchmark suite now prints explicit capabilities (AVX2, AVX-512, AMX, Graviton4) prior to executing search workloads.

### Platform-Specific Behavior Changes
- **AVX-512 Isolation**: Separated the experimental `__AVX512F__` search logic out of the main compiler path into `keystone_avx512.c`, compiling securely with `-mavx512f -mavx512dq`.
- **AMX Constraints**: Bounded AMX usage strictly to "feature detection only," awaiting tile-stride integer search correctness validation.
- **Graviton4 Profiling**: Explicitly introduced Neoverse V2 caching topology logic locking anchors directly to the 2MB private L2 cache.
- **Build Mode Explicit Requirements**: Created robust dependency checks allowing fallback scalar implementations (`KEYSTONE_FORCE_SCALAR`) or fully parallel setups (`KEYSTONE_ENABLE_OPENMP`, `KEYSTONE_ENABLE_FORTRAN`).
