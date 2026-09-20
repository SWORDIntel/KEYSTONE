# KEYSTONE Build Modes

The KEYSTONE search engine is designed to be easily consumed as a library without hiding its native build assumptions. It can be compiled in various configurations depending on your hardware, dependency constraints, and operational requirements.

## 1. Native Default Build (Recommended)
This is the standard build mode for KEYSTONE, leveraging autodetected CPU features like AVX2 and AVX-512 where appropriate, while keeping dependencies minimal.

```bash
make
```
**Features:**
- Compiles with `-march=native -O3` by default.
- AVX-512 experimental features are isolated and compiled if supported.
- `libarchive` and `libzstd` are autodetected via `pkg-config`. If present, the `.tar.zst` extraction paths are enabled automatically.
- **OpenMP is auto-enabled** if the compiler supports it (GCC always does). Set `KEYSTONE_ENABLE_OPENMP=0` to disable.
- **SSE4.2 SIMD path** is compiled when `-msse4.1` is active (via `-march=native` on SSE4.2+ CPUs). This provides 128-bit integer SIMD for AVX1-only CPUs (Sandy Bridge, Ivy Bridge) that lack AVX2's 256-bit integer ops. Runtime dispatch via `keystone_detect_cpu_features()` selects the best available path.

## 2. Dependency-Minimal (Scalar-Only) Build
If you are deploying KEYSTONE to embedded systems, legacy hardware without SIMD, or environments strictly forbidding vectorization, you can force a purely scalar (C fallback) build.

```bash
make KEYSTONE_FORCE_SCALAR=1
```
**Features:**
- Disables AVX2/AVX-512 explicitly via `-mno-avx2`.
- Avoids all SIMD includes.
- Still utilizes KEYSTONE's core anchor table and interpolation algorithms, guaranteeing sub-logarithmic latency even on scalar paths.

## 3. Archive-Enabled Build (Explicit)
To guarantee that telemetry processing and `.tar.zst` streaming features are compiled (and error out if dependencies are missing), explicitly request it:

```bash
make KEYSTONE_ENABLE_TAR_ZST=1
```
**Features:**
- Requires `libarchive`, `libzstd`, and POSIX threads (`-lpthread`).
- Enables `keystone_tar_zst.c` and `dsmil_telemetry_processor.c`.
- Activates persistent sidecar indexing (`.idx.json`), dual-threaded producer/consumer ring buffering (`enable_pipeline`), and multi-archive parallel batch pools.

## 4. OpenMP Build
OpenMP is now **auto-enabled by default** when the compiler supports it. You no longer need to explicitly request it.

```bash
make    # OpenMP auto-detected and enabled
```

To explicitly enable or disable:
```bash
make KEYSTONE_ENABLE_OPENMP=1   # force enable
make KEYSTONE_ENABLE_OPENMP=0   # force disable
```
**Features:**
- Adds `-fopenmp` to the compiler and linker flags.
- The `auto_backend` router evaluates multi-threaded batch dispatch for batches >= 4096 items (configurable via `KEYSTONE_AUTO_PARALLEL_MIN_ITEMS` at compile time).
- On 8-core Sandy Bridge Xeon: **2x faster batch search** (165 ns/query vs 330 ns/query serial).

## 5. Fortran Scientific Build
For workloads deeply integrated with scientific computing or requiring strict legacy Fortran batch processing pipelines:

```bash
make KEYSTONE_ENABLE_FORTRAN=1
```
**Features:**
- Requires `gfortran`.
- Compiles the Fortran interoperability layer (`libkeystone_batch.so`).
- Instructs the auto-backend router to measure the Fortran execution path during calibration.

## 6. Benchmark-Only Build
To compile just the benchmark suite (without the test suite):

```bash
make benchmarks
```
**Features:**
- Compiles `dsmil_benchmark` and `performance_proof`.
- Useful for validating target-silicon performance before deploying to production.

## 7. Packaging: Shared Library & Static Objects
KEYSTONE provides a first-class Makefile target to build the shared library `libkeystone.so`:

```bash
make lib
```

This compiles all native modules with `-fPIC` and produces `libkeystone.so` with embedded dependency linkage.

**For a Static Archive (`libkeystone.a`):**
```bash
make
ar rcs libkeystone.a src/*.o
```
*(Note: Static linkage allows Link-Time Optimization (LTO) and cross-module SIMD inlining for maximum search throughput.)*

## 8. Vector Similarity Engine Build
The standalone vector similarity engine is built via its self-contained build script:

```bash
./vector_engine/build_vector_engine.sh
```

**Build Features:**
- Detects host compiler (`gcc`, `clang`, `cc`).
- Probes and compiles available hardware kernels:
  - `kernels_scalar.c` (always compiled)
  - `kernels_sse42.c` (compiled on x86 with SSE4.2)
  - `kernels_avx.c` (compiled on x86 with AVX)
  - `kernels_avx2.c` (compiled on x86 with AVX2/FMA)
  - `kernels_avx512.c` (compiled on x86 with AVX-512)
  - `kernels_neon.c` (compiled on ARM aarch64)
- **Compiler-Agnostic OpenMP**: Automatically probes `-fopenmp` using compiler trial builds and links OpenMP for parallel batch search (`keystone_vec_search_batch`).
- **Accelerator Flags**:
  - `KEYSTONE_NO_CUDA=1`: Skip CUDA kernel compilation.
  - `KEYSTONE_HAVE_VPU=1`: Enable Myriad X VPU support.
- Produces `vector_engine/libkeystone_vector.so` with runtime dynamic kernel dispatch.

## 9. Trigram Index Build & Benchmarks
The native C trigram index is built as part of the core library:

```bash
make bin/test_trigram_index
make benchmarks/trigram_benchmark
./benchmarks/trigram_benchmark
```

## 10. Automated Comparative Search Harness
To build the automated comparative search benchmark (`compare_search_auto`):

```bash
make scripts/compare_search_auto
./scripts/compare_search_auto
```
Outputs comprehensive performance metrics, decision provenance, and host/compiler metadata in CSV format.

## 11. Runtime Calibration Environment Variables
The auto-backend selector behavior can be controlled at runtime without recompilation:

| Environment Variable | Default | Purpose |
|---|---|---|
| `KEYSTONE_AUTO_PARALLEL_MIN_ITEMS` | `4096` | Batch size threshold above which OpenMP multi-threading is evaluated. |
| `KEYSTONE_DISABLE_CALIBRATION_CACHE` | `0` | When set to `1`, bypasses the calibration cache and forces fresh candidate calibration on every batch. |
| `KEYSTONE_FORCE_CALIBRATION_FALLBACK` | `0` | When set to `1`, forces candidate measurement failure to verify graceful fallback to static routing policies (`KEYSTONE_DECISION_SOURCE_STATIC_FALLBACK`) without polluting cache. |
| `KEYSTONE_HIT_RATE_PCT` | `100` | Controls target hit percentage (0–100) in benchmark workloads. |
| `KEYSTONE_DATA_GAP` | `1` | Controls data array stride/gap in benchmark workloads. |
| `KEYSTONE_QUERY_STRIDE` | `17` | Controls query key stride pattern in benchmark workloads. |

## 12. Service Daemon Mode (`bin/keystoned`)
To build the unprivileged native service daemon:

```bash
make bin/keystoned
```
**Features:**
- Compiles the standalone daemon supporting local Unix domain sockets (`/run/keystone/keystoned.sock`, mode `0700`).
- Embeds multi-threaded client poll loops, binary IPC framing, and atomic reader-writer generation publication.
- Usage: `bin/keystoned -s /run/keystone/keystoned.sock -d`

## 13. Standalone Trigram Search CLI (`bin/tgrep`)
To build the standalone high-performance grep replacement:

```bash
make bin/tgrep
```
**Features:**
- Compiles `bin/tgrep` utilizing the 24-bit direct directory engine and multi-threaded parallel construction.
- Supports instant persistent index acceleration (`-I` / `.tgrep.idx`), case folding (`-i`), line numbering (`-n`), and thread scaling (`-j <threads>`).

## 14. Federation Intelligence & Comprehensive Test Suite
To compile and execute all 19 test suites across the core engine and all CITADEL Federation Intelligence phases:

```bash
make check
```
Runs:
1. `bin/test_keystone` (Scalar & SIMD core search)
2. `bin/test_keystone_calibration_cache` (Calibration cache & concurrency)
3. `bin/test_keystone_provenance` (Workload shapes & decision provenance)
4. `bin/test_keystone_fortran` (Fortran ABI & adapter)
5. `bin/test_fortran_workloads` (Fortran multi-workload validation)
6. `bin/test_memory_ramp` (Huge pages & bounded memory ramp)
7. `bin/test_cuda_backend` (CUDA & AMX silicon detection)
8. `bin/test_keystone_tar_zst` (Streaming .tar.zst archive reader)
9. `bin/test_trigram_index` (Trigram engine correctness)
10. `bin/test_trigram_sol` (Sol trigram optimization passes)
11. `bin/test_trigram_parallel` (Multi-threaded parallel trigram build)
12. `bin/test_keystone_fabric` (QIHSE capability frame broadcast)
13. `bin/test_keystone_qihse_integration` (QIHSE cluster bus integration)
14. `bin/test_federation_envelope` (Phase 0 wire envelope, dedup ring & checkpoints)
15. `bin/test_exact_temporal_index` (Phase 1 exact identity & monotonic temporal index)
16. `bin/test_keystoned_service` (Phase 2 daemon IPC & security context partitioning)
17. `bin/test_topology_hybrid_planner` (Phase 3 topology cache & two-tier hybrid planner)
18. `bin/test_telemetry_incident_engine` (Phase 4 & 5 telemetry, anomaly & silicon dispatch)
19. `bin/test_federated_query_rag` (Phase 6 & 7 federated coordinator & RAG context packs)

