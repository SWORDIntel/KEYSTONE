# NOT_STISLA Fortran Merge/Walk Matrix Summary

Run date: 2026-04-25T10:39:52Z

Matrix:
- Dataset size: 1,000,000 sorted `int64_t` values
- Query sizes: 8,192, 32,768, 50,000, 200,000
- Hit rate: 100 percent
- Gap profile: dense
- Strides: 1 and 17
- Runs per case: 5
- OpenMP threads: 16
- Completed cases: 8
- Failed cases: 0

Winner counts:
- Auto selector: 6 cases
- Scalar direct loop: 1 case
- Fortran batch: 1 case

Results:

| Case | Queries | Stride | Scalar ns/key | C OpenMP ns/key | Fortran ns/key | Auto ns/key | Winner |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 1 | 8,192 | 1 | 7.21 | 7.66 | 5.28 | 11.23 | Fortran |
| 2 | 8,192 | 17 | 7.25 | 11.59 | 7.75 | 7.60 | Scalar |
| 3 | 32,768 | 1 | 7.10 | 3.83 | 6.30 | 2.58 | Auto |
| 4 | 32,768 | 17 | 7.06 | 5.38 | 10.65 | 3.35 | Auto |
| 5 | 50,000 | 1 | 7.22 | 4.36 | 6.76 | 2.88 | Auto |
| 6 | 50,000 | 17 | 7.59 | 4.66 | 12.34 | 3.26 | Auto |
| 7 | 200,000 | 1 | 7.05 | 3.00 | 7.80 | 2.60 | Auto |
| 8 | 200,000 | 17 | 7.41 | 2.97 | 14.70 | 2.70 | Auto |

Interpretation:
- The new Fortran merge/walk path is benchmark-worthy for dense, sorted, contiguous query streams.
- It is not broad enough for default auto-routing yet.
- Larger batches still favor the C/OpenMP auto path.
- The next Fortran step should expose or detect sorted/contiguous query shape at the C adapter layer so auto can consider it only for the narrow winning profile.

Artifacts:
- `matrix_summary.csv`
- `matrix.log`
- Per-case CSV files in this directory
