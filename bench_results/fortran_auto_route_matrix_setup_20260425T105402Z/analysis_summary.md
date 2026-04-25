# Fortran Auto-Route Matrix Summary

- started_utc: 2026-04-25T10:54:02Z
- benchmark: `scripts/run_perf_matrix.sh`
- build: `NOT_STISLA_ENABLE_FORTRAN=1 ./scripts/build_native.sh`
- cases: 4
- failures: 0

## Findings

- `8192` dense sorted stride-1 queries routed to `fortran` as intended.
- `8192` dense stride-17 queries stayed on `scalar`, preserving the general small-batch fast path.
- `32768` query cases stayed on `c_openmp`, preserving the existing large-batch policy.
- Setup timing is present in the CSV rows and comments. The 1M-row matrix setup totals were roughly `3.3-4.3 ms`.

## Warm-Run Averages

| profile | auto backend | scalar ns/key | openmp ns/key | fortran ns/key | auto ns/key | setup total ms |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| `q8192_h100_dense_s1` | `fortran` | 14.03 | 15.89 | 6.32 | 6.12 | 3.334 |
| `q8192_h100_dense_s17` | `scalar` | 7.03 | 9.44 | 8.44 | 8.02 | 3.458 |
| `q32768_h100_dense_s1` | `c_openmp` | 7.10 | 4.96 | 6.51 | 3.24 | 3.884 |
| `q32768_h100_dense_s17` | `c_openmp` | 7.19 | 5.21 | 10.07 | 4.56 | 4.297 |

## Decision

Keep the C-side query-shape gate narrow: `4096-16384` queries, nondecreasing keys, in-range sampled hits, and average key step no larger than `4`. The current route is validated for the dense sorted case only; broad Fortran routing remains rejected.
