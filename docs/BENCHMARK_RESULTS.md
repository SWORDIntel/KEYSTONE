# KEYSTONE SIMD Benchmark Notes

This file records the intended benchmark posture for SIMD-related work. Treat it as a measurement note, not a universal performance claim.

## Current Implementation Status

| Area | Status |
|---|---|
| AVX2 local scan | Implemented for native x86 builds when AVX2 is compiled and detected at runtime |
| AVX-512 local scan | Build-gated; experimental until measured on target AVX-512 hardware |
| OpenMP batch path | Optional; available when built with OpenMP |
| Fortran batch path | Optional; available when built or auto-enabled by the native toolchain |
| Auto backend calibration | Measures viable local candidates on first cache miss and caches the fastest median timing |
| AMX | Feature detection only; no AMX search backend is currently claimed |

## Measurement Rules

Any SIMD result should include:

- CPU model and microarchitecture;
- compiler and exact compile flags;
- KEYSTONE feature toggles;
- dataset size and distribution;
- query count, query order, and hit rate;
- warmup policy;
- whether the run used scalar, optimized C batch, OpenMP, Fortran, auto-calibrated backend selection, AVX2 local scan, or AVX-512 local scan;
- raw output or CSV from the benchmark run.

## Recommended Commands

Native build and test:

```bash
make clean
make test
```

Scalar comparison build:

```bash
make clean
KEYSTONE_ENABLE_FORTRAN=0 KEYSTONE_ENABLE_TAR_ZST=0 KEYSTONE_FORCE_SCALAR=1 make test
```

Benchmark build:

```bash
make benchmarks
./benchmarks/dsmil_benchmark
./benchmarks/performance_proof
```

## Notes

Older estimates around AVX-512 and engineering-board unlock behavior should not be treated as current project claims unless they are regenerated with the rules above and tied to raw benchmark output.

The defensible claim today is narrower and stronger: KEYSTONE is a native, target-silicon-tuned search library with tested scalar, batch, archive, telemetry, optional Fortran, and optional OpenMP paths, plus host-dependent SIMD local scan support.
