# Dynamic Hot-Path Plan

Benchmark date: 2026-04-25

Machine: Intel Core Ultra 7 165H, AVX2 available, AVX-512 unavailable.

Current validation scope: AVX2-only. The dynamic selector has been built and
smoke-tested on this host only, so near-term work should improve the
architecture-neutral selector behavior before adding AVX-512 or AMX candidate
backends.

Build used for measurements:

```bash
gcc -O3 -march=native -mavx2 -Wall -Wextra -I. -I./include \
  -DNOT_STISLA_ENABLE_FORTRAN -fopenmp \
  scripts/compare_search.c src/not_stisla.c \
  -L./fortran -lnot_stisla_batch -Wl,-rpath,'$ORIGIN/../fortran' \
  -lm -o scripts/compare_search_fortran
```

Once the auto backend API lands, enable its benchmark columns with:

```bash
gcc -O3 -march=native -mavx2 -Wall -Wextra -I. -I./include \
  -DNOT_STISLA_ENABLE_FORTRAN -DNOT_STISLA_BENCH_AUTO=1 -fopenmp \
  scripts/compare_search.c src/not_stisla.c \
  -L./fortran -lnot_stisla_batch -Wl,-rpath,'$ORIGIN/../fortran' \
  -lm -o scripts/compare_search_auto
```

The native build script now builds `scripts/compare_search_auto` directly.
OpenMP is enabled by default for `src/not_stisla.c`; set
`NOT_STISLA_ENABLE_OPENMP=0` to force the scalar fallback build.

Benchmark shape:

- Dataset: sorted `int64_t` array, `n=1,000,000` by default.
- Queries: deterministic mixed hit/miss stream controlled by
  `NOT_STISLA_HIT_RATE_PCT`.
- Data spacing: fixed gap controlled by `NOT_STISLA_DATA_GAP`, with optional
  deterministic nonuniform jitter from `NOT_STISLA_DATA_GAP_JITTER`.
- Hit traversal: deterministic stride controlled by `NOT_STISLA_QUERY_STRIDE`;
  output reports both effective stride and cycle length.
- Threads: `OMP_NUM_THREADS=16` for batch-size sweep.
- Reported values below use warm medians, excluding the first run.

## Results

| Queries | Scalar NOT_STISLA ns/key | C OpenMP batch ns/key | Fortran batch ns/key | Fastest |
| ---: | ---: | ---: | ---: | --- |
| 8 | 15.12 | 3607.13 | 2259.06 | binary/scalar noise floor |
| 32 | 17.92 | 1175.45 | 800.33 | scalar |
| 128 | 9.03 | 233.52 | 185.23 | scalar |
| 512 | 8.25 | 53.66 | 46.58 | scalar |
| 2,048 | 7.87 | 15.99 | 18.06 | scalar |
| 8,192 | 7.54 | 10.00 | 15.45 | scalar |
| 12,000 | 7.23 | 7.89 | 16.09 | scalar |
| 16,000 | 7.12 | 6.77 | 13.52 | C OpenMP batch |
| 24,000 | 7.27 | 5.77 | 11.64 | C OpenMP batch |
| 32,768 | 7.27 | 5.35 | 11.16 | C OpenMP batch |
| 49,152 | 7.71 | 3.50 | 11.35 | C OpenMP batch |
| 65,536 | 7.79 | 4.31 | 12.91 | C OpenMP batch |
| 200,000 | 7.30 | 3.79 | 16.05 | C OpenMP batch |

Thread scaling for 200,000 queries:

| Threads | Scalar ns/key | C OpenMP batch ns/key | Fortran batch ns/key |
| ---: | ---: | ---: | ---: |
| 1 | 7.06 | 9.20 | 51.30 |
| 2 | 7.03 | 7.42 | 30.77 |
| 4 | 7.77 | 5.48 | 22.85 |
| 8 | 7.30 | 4.38 | 19.50 |
| 16 | 7.13 | 3.61 | 16.77 |

## Interpretation

- Fortran is currently not a winning backend for this workload. It beats the C
  OpenMP batch path only when batches are tiny, but scalar NOT_STISLA is still
  much faster in that region.
- The useful dynamic decision is scalar versus C OpenMP batch.
- The observed crossover is near 16,000 queries on this CPU and dataset.
- The first batch run is often much slower, so any automatic selector should
  warm or cache decisions before using timing as policy.

## Proposed Dynamic Hot Paths

Add an explicit auto-selection API:

```c
size_t not_stisla_search_batch_auto(
    const int64_t *arr,
    size_t n,
    not_stisla_batch_item_t *items,
    size_t num_items,
    not_stisla_anchor_table_t *table,
    size_t tol,
    const not_stisla_parallel_config_t *config);
```

Initial routing policy:

- `num_items < 16,000`: use scalar `not_stisla_search` in a tight loop.
- `num_items >= 16,000` and OpenMP is available: use `not_stisla_search_parallel`.
- Fortran stays opt-in through `not_stisla_search_batch_fortran` until a
  workload shows a repeatable win.
- If caller provides `config->num_threads == 1`, prefer scalar unless a local
  calibration says otherwise.

## Improvements To Build Next

1. Add `not_stisla_search_batch_auto` with conservative static thresholds.
2. Add a small calibration cache keyed by CPU feature mask, array-size bucket,
   query-count bucket, and thread count.
3. Measure three candidate paths during warmup: scalar, C batch/OpenMP, and
   optional Fortran.
4. Keep the fastest path for that bucket with hysteresis, for example require a
   10% win before switching.
5. Add benchmark cases for misses, mixed hit rates, sparse data, nonuniform
   telemetry gaps, and cold-cache behavior.
6. Add p50/p95/p99 reporting to `scripts/compare_search.c`; average alone hides
   the first-run and scheduler effects.

## Next Non-AVX/AMX Batch

This batch should stay inside the scalar, C batch, C OpenMP, Fortran-optional,
and benchmark/documentation surface. AVX-512 and AMX implementation work is
deferred until hardware is available for compile, runtime, and benchmark
validation.

Near-term priorities:

1. Expose p95 decision timing publicly.
   The selector already stores p95 internally for calibration. Add it to the
   public decision/reporting surface so benchmarks and callers can distinguish
   a fast-but-jittery backend from a stable one.
2. Add mixed hit/miss workloads.
   Current benchmark results are dense all-hit queries. Add deterministic
   profiles such as 100% hit, 75/25 hit/miss, 50/50 hit/miss, 25/75 hit/miss,
   and 100% miss so calibration does not overfit the all-hit path.
3. Add data gap and stride workloads.
   Cover dense contiguous arrays, sparse arrays with fixed gaps, arrays with
   nonuniform telemetry-style gaps, sequential query strides, random query
   strides, and clustered hot ranges. These profiles should feed both
   benchmark output and cache keys.
4. Improve cache reliability.
   Add explicit workload profile fields to calibration keys, record whether a
   decision came from measured calibration or fallback policy, and add tests
   that repeated calls reuse cached decisions without changing correctness.
5. Keep Fortran explicit for this batch.
   Fortran can remain a measured comparison target when built, but auto-routing
   should continue to select only scalar C or C OpenMP until mixed workload
   results justify expanding the candidate set.

## Current Recommendation

Do not make Fortran the default backend. The next useful implementation is a
dynamic scalar-versus-C-batch selector with Fortran retained as an explicit
experimental backend.

Status: implemented as an in-process calibration cache. The static threshold is
now the fallback policy; the first call for each CPU/array/query/thread bucket
measures scalar C against C OpenMP and caches the faster backend.

## Architecture-Dependent Backend Plan

The selector should treat CPU architecture as a filter for candidate backends,
not as the final decision. Wider features such as AVX-512 or AMX only matter if
the measured workload benefits from the backend that uses them.

### Backend Candidate Matrix

| Backend | Build Gate | Runtime Gate | Good Candidate For | Default Status |
| --- | --- | --- | --- | --- |
| Scalar C loop | Always | Always | Small batches, single-thread callers, calibration fallback | Always enabled |
| C batch scan | Always | Always | Sorted query batches, low overhead contiguous work | Enabled |
| C OpenMP batch | `_OPENMP` | `num_items` large enough, threads > 1 | Large batches where thread startup is amortized | Enabled when OpenMP exists |
| C AVX2 search | Compiler emits AVX2-safe objects | `NOT_STISLA_CPU_AVX2` | Current x86 hot path | Enabled on this host |
| C AVX-512 search | Separate AVX-512 object or guarded compile flag | `NOT_STISLA_CPU_AVX512` plus OS state support | Large vector compare windows and batch kernels | Candidate only |
| C AMX path | Separate AMX object | `NOT_STISLA_CPU_AMX` plus OS tile state support | Only if a tiled compare/filter algorithm is added | Candidate only |
| Fortran batch | `NOT_STISLA_ENABLE_FORTRAN=1` | `not_stisla_fortran_backend_available()` | Workloads where Fortran/OpenMP beats C in calibration | Explicit candidate |

### Required Design Rule

Do not compile one binary that can execute unsupported instructions by accident.
AVX-512 and AMX code should live in isolated translation units or functions with
compiler target attributes, and the dispatch layer should call them only after
runtime feature detection confirms CPU and OS support.

This avoids the `SIGILL` failure observed when the native build used
unconditional AVX-512 flags on AVX2-only hardware.

### Auto Selector API

Add an explicit auto path:

```c
typedef enum not_stisla_backend {
    NOT_STISLA_BACKEND_AUTO = 0,
    NOT_STISLA_BACKEND_SCALAR,
    NOT_STISLA_BACKEND_C_BATCH,
    NOT_STISLA_BACKEND_C_OPENMP,
    NOT_STISLA_BACKEND_C_AVX2,
    NOT_STISLA_BACKEND_C_AVX512,
    NOT_STISLA_BACKEND_C_AMX,
    NOT_STISLA_BACKEND_FORTRAN
} not_stisla_backend_t;

typedef struct not_stisla_backend_decision {
    not_stisla_backend_t backend;
    uint32_t cpu_features;
    size_t array_size_bucket;
    size_t query_count_bucket;
    int thread_count;
    double estimated_ns_per_key;
    double p95_ns_per_key;
} not_stisla_backend_decision_t;

size_t not_stisla_search_batch_auto(
    const int64_t *arr,
    size_t n,
    not_stisla_batch_item_t *items,
    size_t num_items,
    not_stisla_anchor_table_t *table,
    size_t tol,
    const not_stisla_parallel_config_t *config);

int not_stisla_get_last_backend_decision(
    not_stisla_backend_decision_t *decision);
```

The `get_last_backend_decision` call makes benchmark runs auditable and prevents
the auto selector from becoming a black box.

### Benchmark Harness Support

`scripts/compare_search.c` now reserves CSV columns for the auto selector:

- `auto_batch_ns`
- `auto_backend_bench`
- `auto_decision_available`
- `auto_backend`
- `auto_cpu_features`
- `auto_array_bucket`
- `auto_query_bucket`
- `auto_thread_count`
- `auto_estimated_ns_per_key`
- `auto_p95_ns_per_key`

The native build emits `scripts/compare_search_auto` with
`-DNOT_STISLA_BENCH_AUTO=1`, so it calls `not_stisla_search_batch_auto()` and
then `not_stisla_get_last_backend_decision()`. Benchmark output records both
the measured auto path and the backend selected by the dispatcher. Direct
manual builds can still leave `NOT_STISLA_BENCH_AUTO=0` to emit disabled auto
values.

Workload coverage knobs:

- `NOT_STISLA_HIT_RATE_PCT`: deterministic mixed hit/miss ratio, for example
  100, 75, 50, 25, or 0.
- `NOT_STISLA_DATA_GAP`: fixed positive spacing between sorted data values.
- `NOT_STISLA_DATA_GAP_JITTER`: deterministic extra per-row spacing for sparse
  nonuniform telemetry-style arrays.
- `NOT_STISLA_QUERY_STRIDE`: hit-index stride; CSV comments report
  `query_effective_stride` and `query_cycle` so stride aliasing is visible.
- `NOT_STISLA_PROFILE`: free-form profile label emitted in CSV comments; the
  matrix runner sets this to a deterministic case name.

### Full Benchmark Matrix Runner

Use `scripts/run_perf_matrix.sh` to run repeatable full-sweep benchmark cases
against `scripts/compare_search_auto`. The runner writes one CSV per case and a
concise manifest:

```bash
NOT_STISLA_ENABLE_FORTRAN=1 ./scripts/build_native.sh
./scripts/run_perf_matrix.sh
```

Default matrix:

- Data sizes: `NOT_STISLA_MATRIX_N="1000000"`.
- Query sizes: `NOT_STISLA_MATRIX_QUERIES="512 8192 32768 200000"`.
- Hit rates: `NOT_STISLA_MATRIX_HIT_RATES="100 75 50 25 0"`.
- Gap profiles: `NOT_STISLA_MATRIX_GAPS="dense:1:0 sparse:16:0 jitter:8:8"`.
- Strides: `NOT_STISLA_MATRIX_STRIDES="1 17 257"`.
- Runs per case: `NOT_STISLA_MATRIX_RUNS=7`.
- Threads: `NOT_STISLA_MATRIX_THREADS`, defaulting to `OMP_NUM_THREADS` or 16.

Runtime controls:

- Set `NOT_STISLA_MATRIX_LIMIT=N` to stop after `N` generated cases for smoke
  runs.
- Set `NOT_STISLA_MATRIX_OUT=/path/to/output` to choose the output directory.
- Set `NOT_STISLA_BENCH_BIN=/path/to/compare_search_auto` to benchmark another
  compatible binary.
- Override any matrix list above with space-separated values.

Small smoke example:

```bash
NOT_STISLA_MATRIX_OUT=/tmp/not_stisla_smoke \
NOT_STISLA_MATRIX_N="10000" \
NOT_STISLA_MATRIX_QUERIES="64 256" \
NOT_STISLA_MATRIX_HIT_RATES="100 50" \
NOT_STISLA_MATRIX_GAPS="dense:1:0 jitter:4:2" \
NOT_STISLA_MATRIX_STRIDES="1 17" \
NOT_STISLA_MATRIX_RUNS=2 \
NOT_STISLA_MATRIX_THREADS=2 \
NOT_STISLA_MATRIX_LIMIT=3 \
./scripts/run_perf_matrix.sh
```

The runner prints:

- `case ...: /path/to/case.csv` for each completed case.
- `summary_csv=/path/to/matrix_summary.csv`.
- `run_log=/path/to/matrix.log`.

The full default matrix is intentionally more expensive than the smoke command:
`1 * 4 * 5 * 3 * 3 = 180` benchmark processes before any user overrides.

Auto selector reporting columns:

- `auto_backend`
- `auto_cpu_features`
- `auto_array_bucket`
- `auto_query_bucket`
- `auto_thread_count`
- `auto_estimated_ns_per_key`
- `auto_p95_ns_per_key`

### Static Policy Before Calibration

Use a conservative initial policy before enough measurements are available:

- `num_items < 16,000`: scalar loop on this AVX2 host.
- `num_items >= 16,000`: C OpenMP batch when OpenMP is available and thread
  count is greater than one.
- AVX-512 backend: candidate for large batches only after an AVX-512 object path
  exists and calibration beats C OpenMP by at least 10%.
- AMX backend: disabled until a real tiled integer search design exists.
- Fortran backend: candidate only when built, and selected only if calibration
  beats the best C path by at least 10%.

The implemented selector calibrates scalar versus C OpenMP on first use of each
bucket. The first benchmark row for a new bucket includes calibration overhead;
later rows use the cached decision. Because the selector uses live timing, it
may choose C OpenMP below the old 16,000-query static threshold when that is
faster locally.

### Calibration Cache

Cache decisions by:

- CPU feature mask.
- Backend build mask.
- Array-size bucket: powers of two are enough initially.
- Query-count bucket: powers of two are enough initially.
- Thread count.
- Workload profile: dense hits, mixed hits/misses, sparse gaps, telemetry gaps.

Each cache entry should store:

- Chosen backend.
- Warm median ns/key.
- p95 ns/key.
- Sample count.
- Timestamp or generation counter.
- Whether the entry came from static policy or measured calibration.

Implemented cache scope:

- CPU feature mask.
- Array-size bucket.
- Query-count bucket.
- Thread count.
- Chosen backend.
- Measured selected-path `estimated_ns_per_key`.

The current cache is process-local and fixed size. It does not persist across
processes.

### Calibration Method

For a new bucket:

1. Build a small representative sample from the caller's query batch.
2. Run scalar, C batch/OpenMP, and any compiled architecture candidates.
3. Run Fortran only if `NOT_STISLA_ENABLE_FORTRAN=1`.
4. Exclude the first run from timing.
5. Pick a new backend only if it beats the current best by at least 10%.
6. Record p95 as well as median so jitter-prone paths are penalized.

Current implementation:

- Samples up to 32,768 query items from the caller's batch.
- Measures scalar C and C OpenMP on copied item arrays so caller results are not
  mutated during calibration.
- Runs one warmup pass per candidate, then uses the median of three measured
  passes for selection.
- Stores p95 for the cached decision and exposes it in benchmark output.
- Keeps Fortran out of auto-selection for now.
- Requires C OpenMP to beat scalar by at least 10% before selecting it.
- Records the actual selected-path time in `estimated_ns_per_key`.

Remaining gap: workload profile fields are not part of the public decision
struct or cache key yet, so benchmark output must carry hit-rate, gap, jitter,
and stride context alongside selector decisions.

### AVX-512 Work Plan

Deferred until AVX-512 hardware is available. Do not implement or enable this
candidate from AVX2-only validation alone.

1. Split AVX-512 code into a separate object, for example
   `src/not_stisla_avx512.c`.
2. Compile that object only with AVX-512 flags.
3. Expose an internal function such as
   `not_stisla_search_batch_avx512_candidate`.
4. Guard calls with `NOT_STISLA_CPU_AVX512` and OS extended-state checks.
5. Add benchmark rows for AVX2 C batch, AVX-512 C batch, and Fortran batch on
   AVX-512 hardware.

### AMX Work Plan

Deferred until AMX hardware is available. Do not implement or expose an AMX
candidate from AVX2-only validation alone.

AMX should not be assumed useful for sorted integer search. Use it only if a
specific tiled algorithm is designed and measured.

Candidate design to test:

1. Tile query keys and array windows into blocks.
2. Use AMX only for compare/filter stages if the instruction set supports the
   needed integer operations efficiently.
3. Fall back to AVX-512 or scalar verification for exact index resolution.
4. Benchmark against AVX-512 C batch before exposing it as a selector candidate.

If the tiled design cannot beat AVX-512 on real hardware, keep AMX out of the
runtime selector.

### Fortran Policy

Fortran remains valuable as a second implementation for comparison and possible
architecture-specific wins, but it should stay an optional dependency:

- Build with `NOT_STISLA_ENABLE_FORTRAN=1`.
- Include it in calibration only when linked.
- Do not route to Fortran based only on CPU features.
- Require a measured win over scalar, C OpenMP, and AVX candidates.
- Keep the C API stable if Fortran is missing.

### Implementation Phases

1. Add backend enum, decision struct, and `not_stisla_search_batch_auto`. Done.
2. Implement static AVX2-host policy using the current 16k threshold. Done as fallback.
3. Add last-decision reporting and benchmark output for selected backend. Done.
4. Add measured calibration cache for scalar and C OpenMP. Done.
5. Expand calibration to warmup-excluded median/p95 sampling. Done.
6. Expose calibration p95 in the public decision struct. Done.
7. Add mixed hit/miss benchmark profiles. Done for deterministic hit-rate
   sweeps; calibration cache keys still need workload fields.
8. Add data gap and stride workload profiles. Done for fixed/nonuniform gaps
   and deterministic stride; random and clustered profiles remain open.
9. Add workload profile fields to calibration cache keys and decision output.
10. Add cache reliability tests for reuse, fallback policy, and correctness.
11. Add optional Fortran calibration candidate, still disabled by default.
12. Refactor AVX-512 into an isolated candidate object only after AVX-512
    hardware is available.
13. Re-benchmark on real AVX-512 hardware and update thresholds.
14. Investigate AMX only after AMX hardware is available and the AVX-512
    candidate is measured.
