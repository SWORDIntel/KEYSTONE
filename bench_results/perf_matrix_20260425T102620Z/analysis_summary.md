# NOT_STISLA Perf Matrix Summary

Run date: 2026-04-25T10:26:20Z

Matrix:
- Dataset size: 1,000,000 sorted `int64_t` values
- Query sizes: 512, 8,192, 32,768, 200,000
- Hit rates: 100, 75, 50, 25, 0 percent
- Gap profiles: dense, sparse, jittered
- Strides: 1, 17, 257
- Runs per case: 7
- OpenMP threads: 16
- Completed cases: 180
- Failed cases: 0

Backend availability:
- Scalar C: available
- C OpenMP batch: available
- Auto selector: available
- Fortran batch: unavailable locally because `gfortran` is not installed

Winner counts:
- Scalar direct loop: 94 cases
- Auto batch selector: 80 cases
- Direct C OpenMP batch: 6 cases

Auto selector decisions:
- Scalar backend selected: 90 cases
- C OpenMP backend selected: 90 cases

Median by query size:

| Queries | Scalar ns/key | C OpenMP batch ns/key | Auto ns/key | Auto Backend | Main Result |
| ---: | ---: | ---: | ---: | --- | --- |
| 512 | 6.16 | 55.41 | 8.97 | scalar | Scalar direct loop dominates; batch API overhead is visible. |
| 8,192 | 5.70 | 8.68 | 7.20 | scalar | Scalar remains best for most cases; OpenMP wins a few high-cost profiles. |
| 32,768 | 5.24 | 4.48 | 3.05 | C OpenMP | Auto/OpenMP is the useful path. |
| 200,000 | 5.17 | 2.61 | 2.33 | C OpenMP | Auto/OpenMP is consistently the useful path. |

Interpretation:
- The current 16K query threshold is a reasonable conservative split.
- Lowering the OpenMP threshold to 8K would improve a few cases but regress most 8K cases.
- Small batches should keep using scalar paths; the batch API has unavoidable result-writing overhead compared with the direct scalar loop benchmark.
- The next selector improvement should not be a single lower threshold. It should use workload hints or observed per-bucket feedback if callers can expose query shape.

Artifacts:
- `matrix_summary.csv`
- `matrix.log`
- Per-case CSV files in this directory
