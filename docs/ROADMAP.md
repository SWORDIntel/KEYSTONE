# KEYSTONE Roadmap

This roadmap keeps the README ambition tied to work that can be built, tested,
measured, and defended. KEYSTONE should remain a native, silicon-tuned indexing
component, but the project needs stronger evidence around when each execution
path is selected and why.

## North Star

KEYSTONE should become a small, auditable search and indexing engine for sorted
`int64_t` datasets where backend choice is local, measurable, and explainable
across the resident compute fabric.

The target user is not looking for a general database replacement. The target
user needs a component that can sit next to larger storage, telemetry, archive,
or investigative systems and provide repeatable indexed lookup with clear
performance behavior on the resident host.

## Principles

- Native first: optimize for the resident silicon, compiler, runtime, and
  dependency set that will actually run the workload. CPU paths are implemented
  today; GPU and NPU paths are future backend families only after explicit
  implementations and data-movement-aware measurement.
- Measured decisions: backend selection should be based on local evidence when
  the workload is large enough to justify calibration.
- Explainable dispatch: every automatic backend decision should expose the
  selected backend, decision source, query shape, timing, and calibration scope.
- Conservative claims: experimental hardware paths stay marked experimental
  until they have target-silicon benchmark evidence.
- Visible compliance: licensing and commercial-use boundaries should be handled
  through the AGPL notice, explicit licensing terms, and auditable project
  documentation, not covert network behavior.

## Phase 1: Backend Decision Transparency

Status: completed.

Goal: make `keystone_search_batch_auto()` decisions inspectable enough that a
benchmark or embedding application can explain why a backend was used.

Acceptance gates:

- Public decision metadata reports backend, CPU feature mask, size bucket, query
  bucket, thread count, median/p95 timing, query shape, decision source,
  calibration run count, and candidate count.
- Public label helpers translate backend, decision source, and query shape
  values into stable strings for benchmarks and integration logs.
- Tests cover fast-path, measured, cached, and fallback-style decision records.
- README and benchmark docs describe what each decision field means.
- No hidden outbound networking or implicit licensing enforcement exists in the
  dispatch path.
- Emit decision records from benchmark binaries in a stable machine-readable
  format.
- Add cache-hit correctness tests that compare result arrays before and after
  calibration reuse.

Next work:

- None (Phase completed).

## Phase 2: Workload-Aware Calibration

Status: completed.

Goal: move from coarse calibration buckets to workload-aware buckets without
making the selector fragile or expensive.

Acceptance gates:

- Calibration keys include stable workload descriptors such as hit-rate class,
  query order, gap profile, and stride profile.
- Cold cache, warm cache, all-hit, mixed-hit, and all-miss profiles can be
  compared without changing source code.
- Cache entries include enough provenance to reject misleading reuse across
  incompatible workload shapes.
- Selector overhead remains bounded and is bypassed for workloads too small to
  amortize calibration.
- Define public workload-shape enums.
- Add profile detection for sorted, strided, random, dense, sparse, and mixed
  hit-rate query batches.
- Extend benchmark fixtures to generate repeatable profile combinations.

Next work:

- None (Phase completed).

## Phase 3: Benchmark Evidence Pipeline

Status: completed.

Goal: make KEYSTONE performance claims reproducible from raw local runs rather
than README prose.

Acceptance gates:

- Standard benchmark output includes CSV or JSON rows with host, compiler,
  feature toggles, dataset profile, backend decision metadata, throughput,
  median latency, p95 latency, RSS, and page-fault counters where available.
- `scripts/run_perf_matrix.sh` produces a single results directory containing
  raw logs, normalized tables, and a host summary.
- Benchmark docs distinguish current measured results from future targets.
- At least one scalar-only comparison path is kept available for every benchmark
  profile.
- Add a shared benchmark result writer.
- Add `perf stat` integration behind an explicit opt-in flag.
- Add memory-ramp reporting for 5%, 10%, 25%, 40%, and 60% of `MemAvailable`.

Next work:

- None (Phase completed).

## Phase 4: Archive and Telemetry Integration

Status: completed.

Goal: make `.tar.zst` ingestion and telemetry processing feel like first-class
integration paths instead of side utilities.

Acceptance gates:

- Archive member validation is documented and tested for malformed, missing, and
  unsupported member types.
- Telemetry examples show how records become sorted lookup keys and how lookup
  failures are reported.
- Large archive profiles are benchmarked separately from in-memory synthetic
  arrays.
- Operational guidance covers untrusted input handling, audit trails, and
  separation of benchmark data from production data.

Next work:

- None (Phase completed).

## Phase 5: Platform-Specific Backends

Status: completed.

Goal: keep platform-specific paths powerful but honest, whether the backend is
CPU SIMD, CPU parallelism, Fortran, GPU, or NPU.

Acceptance gates:

- AVX-512 code is isolated into a separately compiled object with AVX-512 flags
  and validated on real AVX-512 hardware.
- AMX remains feature-detection-only until there is a real tiled integer search
  design and benchmark evidence.
- OpenMP and Fortran routing expands only when they beat scalar/C batch paths by
  at least 10% in multiple real workload buckets.
- GPU and NPU backends are not claimed until they include explicit transfer-cost
  accounting, correctness tests, fallback behavior, and decision provenance.
- Platform notes describe compiler, CPU, accelerator, dependency, runtime, and
  container constraints.

Next work:

- None (Phase completed).

## Phase 6: Packaging and Integration Contracts

Status: completed.

Goal: make KEYSTONE easier to consume as a library without hiding its native
build assumptions.

Acceptance gates:

- Build modes are documented for scalar-only, native default, OpenMP, Fortran,
  archive-enabled, benchmark-only, and dependency-minimal builds.
- Public headers have stable comments for decision metadata and optional
  features.
- Integration docs show database-adjacent usage without implying KEYSTONE is a
  full database.
- Release notes separate API changes, benchmark changes, and platform-specific
  behavior changes.

Next work:

- None (Phase completed).
