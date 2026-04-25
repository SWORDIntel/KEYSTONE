# NOT_STISLA Fortran Backend Plan

This plan covers the optional Fortran batch-search backend under `fortran/`.
The current prototype is intentionally separate from the main C API until the
ABI, build path, correctness coverage, and benchmark value are proven.

## Current State

- `fortran/not_stisla_batch.f90` exports `not_stisla_batch_search_i64`.
- `fortran/not_stisla_batch_backend.h` declares the raw C ABI symbol.
- `not_stisla_search_batch_fortran` adapts `not_stisla_batch_item_t` batches
  to the Fortran backend when compiled with `NOT_STISLA_ENABLE_FORTRAN`.
- `not_stisla_fortran_backend_available` reports whether the current binary was
  compiled with Fortran support.
- The exported function accepts sorted `int64_t` data, a list of keys, and an
  output index array.
- Misses are reported as `-1`; hits are zero-based indices.
- The Fortran backend currently performs independent binary searches for each
  key and can use OpenMP when compiled with `-fopenmp`.
- The main C API already has `not_stisla_search_batch` and
  `not_stisla_search_parallel`; the Fortran path is exposed as an explicit
  optional adapter rather than a transparent replacement.

## Goal

Add Fortran as an optional batch execution backend for high-volume query
streams without changing the stable scalar C API. The backend should only be
enabled when it is built, linked, tested, and benchmarked against the existing C
batch paths.

## Phase 1: Stabilize The ABI

- Add a small C header for the Fortran symbol, for example
  `fortran/not_stisla_batch_backend.h`. Status: done.
- Use C-compatible fixed-width types only: `int64_t`, `size_t`, and pointer
  arguments.
- Keep the return contract simple: write one `int64_t` result per key, using
  `-1` for misses.
- Add an optional status-returning wrapper in C rather than changing the raw
  Fortran symbol.
- Document that inputs must be sorted ascending and remain valid for the full
  call.

## Phase 2: Add A C Adapter

- Add an adapter such as `not_stisla_search_batch_fortran`. Status: done.
- Convert `not_stisla_batch_item_t` keys into a contiguous `int64_t` key array.
- Call `not_stisla_batch_search_i64`.
- Convert returned `int64_t` indices back to `not_stisla_result_t`.
- Preserve original item order and `ordinal` behavior.
- Do not mutate anchor-table state in the first adapter version; measure the
  backend as a pure stateless batch path first.

## Phase 3: Build Integration

- Add an opt-in build flag, for example `NOT_STISLA_ENABLE_FORTRAN=1`. Status:
  done.
- Compile `fortran/not_stisla_batch.f90` only when `gfortran` is available.
- Support two modes:
  `single-threaded` without OpenMP, and `openmp` with `-fopenmp`.
- Keep the default build C-only.
- Ensure missing Fortran tooling fails with a clear message only when the
  Fortran flag is enabled.

## Phase 4: Correctness Tests

- Add C tests that call the adapter with:
  sorted hits, misses, duplicate query keys, empty key batches, empty datasets,
  first element, last element, negative values, and large `int64_t` values.
- Cross-check every Fortran result against the existing scalar C search.
- Test non-multiple batch sizes, including `1`, `2`, `3`, `7`, `64`, `65`, and
  at least one large batch.
- Run tests both with and without the Fortran build flag where available.
  Status: basic coverage added in `tests/test_fortran_backend.c`.

## Phase 5: Benchmarks

- Extend `scripts/compare_search.c` or `scripts/benchmark.c` with a Fortran
  batch mode. Status: basic `scripts/compare_search.c` coverage added.
- Report C scalar, C batch, C OpenMP batch, Fortran single-threaded batch, and
  Fortran OpenMP batch.
- Use fixed datasets with documented size, distribution, hit rate, compiler,
  flags, CPU model, and thread count.
- Report median and p95 latency, not only best-case throughput.
- Keep results separate from projections.
- Current dense-hit benchmark results are recorded in
  `docs/DYNAMIC_HOT_PATH_PLAN.md`.

## Phase 6: Adoption Gate

Only wire the Fortran path into a public API selection mechanism after all of
these are true:

- Correctness tests pass against the C reference path.
- The build is optional and does not break C-only users.
- Benchmarks show a repeatable gain for at least one real batch workload.
- Threading behavior is documented and does not oversubscribe caller-managed
  parallelism.
- Error handling is clear for null pointers, empty arrays, missing libraries,
  and unsupported build configurations.

## Near-Term Implementation Order

1. Expand benchmark coverage beyond dense all-hit queries.
2. Add dynamic scalar-versus-C-batch hot-path selection.
3. Expand test coverage across larger batch sizes and OpenMP thread counts.
4. Decide whether the Fortran path should remain explicit or become selectable
   through a backend-selection API.
