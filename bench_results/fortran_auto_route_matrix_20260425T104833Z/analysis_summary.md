# Fortran Auto-Route Matrix Summary

- started_utc: 2026-04-25T10:48:33Z
- benchmark: `scripts/run_perf_matrix.sh`
- build: `NOT_STISLA_ENABLE_FORTRAN=1 ./scripts/build_native.sh`
- cases: 4
- failures: 0

## Findings

- `8192` dense sorted stride-1 queries routed to `fortran` as intended.
- `8192` dense stride-17 queries stayed on `scalar`, so the Fortran exception did not catch the wider-stride general case.
- `32768` query cases stayed on `c_openmp`, preserving the existing large-batch policy.
- Warm-run average for the intended Fortran route was `3.46 ns/key` through auto versus `8.25 ns/key` scalar and `4.09 ns/key` direct Fortran.

## Warm-Run Averages

| profile | auto backend | scalar ns/key | openmp ns/key | fortran ns/key | auto ns/key |
| --- | --- | ---: | ---: | ---: | ---: |
| `q8192_h100_dense_s1` | `fortran` | 8.25 | 10.02 | 4.09 | 3.46 |
| `q8192_h100_dense_s17` | `scalar` | 14.88 | 11.22 | 14.57 | 16.39 |
| `q32768_h100_dense_s1` | `c_openmp` | 7.91 | 5.06 | 6.22 | 3.68 |
| `q32768_h100_dense_s17` | `c_openmp` | 7.45 | 4.94 | 11.28 | 4.27 |

## Decision

Keep the C-side query-shape gate narrow: `4096-16384` queries, nondecreasing keys, in-range sampled hits, and average key step no larger than `4`. Do not broaden Fortran routing until it wins additional real workload buckets.
