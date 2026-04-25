# NOT_STISLA Improvement Plan

This roadmap focuses on practical, measurable optimization work. Each item should land with correctness tests, repeatable benchmarks, and clear fallback behavior on unsupported hardware.

## 1. Stabilize the Baseline

- Keep one documented scalar reference path for correctness checks.
- Define benchmark datasets by size, distribution, hit rate, and cache residency.
- Report median, p95, best, and worst latency with CPU model, compiler, flags, and thread count.
- Separate measured results from projections.

## 2. Tighten the Hot Search Loop

- Audit branch behavior and remove avoidable unpredictable branches.
- Keep SIMD paths small, feature-gated, and covered by scalar cross-checks.
- Tune prefetch distance per workload instead of using one global distance.
- Avoid extra work in the small-array path where call overhead and branching dominate.

## 3. Improve Memory Behavior

- Align large arrays where practical and document ownership requirements.
- Gate huge-page hints behind size thresholds and treat failures as non-fatal.
- Measure cache and TLB behavior with `perf stat` before accepting changes.
- Prefer contiguous batch processing for query streams that can amortize setup cost.

## 4. Add Batch-Oriented Execution

- Keep the current scalar C API stable.
- Prototype a separate batch API that writes one result per key using zero-based indices and `-1` for misses.
- Compare single-threaded batch, OpenMP batch, and caller-managed threading.
- Wire batch support into the main API only after the ABI, error handling, and benchmarks are settled.

## 5. Portability and Release Hygiene

- Preserve runtime CPU feature detection and safe fallbacks.
- Keep optional backends isolated until they have build-system support and CI coverage.
- Document compiler flags and unsupported configurations explicitly.
- Publish benchmark artifacts with enough detail to reproduce or reject claimed gains.
