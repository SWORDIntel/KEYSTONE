# NOT_STISLA Fortran-Enabled Matrix Summary

Run date: 2026-04-25T10:31:39Z

Matrix:
- Dataset size: 1,000,000 sorted `int64_t` values
- Query sizes: 512, 8,192, 32,768, 200,000
- Hit rates: 100, 50, 0 percent
- Gap profiles: dense and jittered
- Stride: 17
- Runs per case: 5
- OpenMP threads: 16
- Completed cases: 24
- Failed cases: 0

Backend availability:
- Scalar C: available
- C OpenMP batch: available
- Auto selector: available
- Fortran batch: available

Winner counts:
- Scalar direct loop: 12 cases
- Auto selector: 12 cases
- Direct C OpenMP batch: 0 cases
- Fortran batch: 0 cases

Median by query size:

| Queries | Scalar ns/key | C OpenMP batch ns/key | Fortran batch ns/key | Auto ns/key | Main Result |
| ---: | ---: | ---: | ---: | ---: | --- |
| 512 | 5.47 | 51.91 | 40.44 | 6.82 | Scalar wins; Fortran/OpenMP overhead dominates. |
| 8,192 | 5.14 | 7.27 | 12.45 | 5.95 | Scalar still wins. |
| 32,768 | 5.32 | 4.49 | 10.77 | 2.42 | Auto/OpenMP wins; Fortran remains too slow. |
| 200,000 | 5.46 | 2.69 | 11.94 | 2.50 | Auto/OpenMP wins; Fortran remains too slow. |

Interpretation:
- The current Fortran backend is correct, but it should remain explicit and experimental.
- It does not justify auto-routing in the current form because it performs independent binary searches per key.
- The useful path is still scalar for small batches and C/OpenMP through auto for larger batches.
- A future Fortran backend would need a different algorithm, such as interpolation-aware batch search or a sorted-query merge strategy, before it can compete.

Artifacts:
- `matrix_summary.csv`
- `matrix.log`
- Per-case CSV files in this directory
