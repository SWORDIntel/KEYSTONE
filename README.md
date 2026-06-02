# KEYSTONE

[![C](https://img.shields.io/badge/C-11-blue.svg)](https://en.wikipedia.org/wiki/C11_(C_standard_revision))
[![Fortran](https://img.shields.io/badge/Fortran-90+-purple.svg)](https://en.wikipedia.org/wiki/Fortran)
[![Python](https://img.shields.io/badge/Python-3-yellow.svg)](https://www.python.org/)
[![License](https://img.shields.io/badge/License-AGPL--3.0-red.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Linux-green.svg)](https://www.kernel.org/)

High-performance interpolation search and batch lookup engine for sorted `int64_t` datasets. KEYSTONE combines anchor-guided interpolation search with runtime SIMD detection (AVX2 / AVX-512), OpenMP parallelism, an optional Fortran batch backend, and `.tar.zst` archive ingestion.

---

## Architecture

```mermaid
flowchart TB
    subgraph Search["Search Pipeline"]
        Q[Query Key] --> Auto{Auto Backend}
        Auto -->|Small / Scalar| S[Scalar Interpolation]
        Auto -->|AVX2| A2[AVX2 Batch]
        Auto -->|AVX-512| A5[AVX-512 Batch]
        Auto -->|OpenMP| MP[Parallel Batch]
        Auto -->|Fortran| FT[Fortran Batch]
        S --> AT[Anchor Table]
        A2 --> AT
        A5 --> AT
        MP --> AT
        FT --> AT
        AT --> R[Result Index]
    end

    subgraph Ingestion["Data Ingestion"]
        RAW[Raw CSV/JSON/Text] --> TZST[.tar.zst Archive]
        TZST --> TP[Telemetry Processor]
        TP --> IDX[Member Offset Index]
        IDX --> AT
    end

    subgraph Config["Runtime Tuning"]
        CPU[CPU Feature Detect] --> Auto
        WL[Workload Type] --> CFG[Config Selector]
        CFG --> S
        CFG --> A2
        CFG --> A5
    end
```

---

## Quick Start

### Build the Library

```bash
./build.sh
```

This produces `libkeystone.so` in the repository root with auto-detected AVX2/AVX-512, OpenMP, Fortran, and `tar.zst` support.

### Build Tests & Benchmarks

```bash
make clean && make all
```

### Run the Benchmark Suite

```bash
./benchmark.sh
```

Results are saved to:
- `benchmark_results.csv` — raw per-profile measurements
- `benchmark_comparison.png` — aggregated latency & speedup chart

### Run Tests

```bash
./bin/test_core_native
./bin/test_auto_backend
./bin/test_fortran_backend
./bin/test_telemetry_processor_perf
./bin/test_tar_zst
./bin/test_performance_fix
./bin/test_enhanced
```

---

## Project Layout

```mermaid
flowchart LR
    Root[KEYSTONE Root] --> S[src/]
    Root --> I[include/]
    Root --> T[tests/]
    Root --> B[benchmarks/]
    Root --> D[docs/]
    Root --> F[fortran/]
    Root --> SC[scripts/]

    S --> C1[keystone.c]
    S --> C2[dsmil_keystone_wrapper.c]
    S --> C3[dsmil_telemetry_processor.c]
    S --> C4[keystone_tar_zst.c]

    I --> H1[keystone.h]
    I --> H2[dsmil_telemetry_processor.h]
    I --> H3[dsmil_keystone_wrapper.h]

    T --> E1[test_core_native]
    T --> E2[test_tar_zst]
    T --> E3[test_auto_backend]

    B --> BM1[dsmil_benchmark]
    B --> BM2[performance_proof]
    B --> VIZ[visualize_benchmarks.py]

    F --> F90[keystone_batch.f90]
    F --> FSO[libkeystone_batch.so]

    SC --> BN[build_native.sh]
    SC --> CMP[compare_search.c]
    SC --> CHART[generate_benchmark_chart.py]
```

---

## Core API

### Single Search

```c
#include "keystone.h"

keystone_anchor_table_t* table = keystone_anchor_table_create();
keystone_result_t idx = keystone_search(data, n, key, table, 0);
if (idx != KEYSTONE_NOT_FOUND) { /* found at idx */ }
keystone_anchor_table_destroy(table);
```

### Enhanced / Tuned Search

```c
keystone_config_t cfg;
keystone_config_init(&cfg, KEYSTONE_WORKLOAD_TELEMETRY);
keystone_get_tuned_config(n, &cfg);

keystone_result_t idx = keystone_search_enhanced(data, n, key, table, &cfg);
```

### Batch Search

```c
keystone_batch_item_t batch[num_queries];
for (size_t i = 0; i < num_queries; ++i) batch[i].key = queries[i];

size_t found = keystone_search_batch(data, n, batch, num_queries, table, 8, NULL);
```

### Auto Backend Batch

```c
keystone_parallel_config_t pc = { .num_threads = 8, .batch_chunk = 256 };
size_t found = keystone_search_batch_auto(data, n, batch, num_queries, table, 8, &pc);
```

### tar.zst Archive Search

```c
#include "dsmil_keystone_wrapper.h"

dsmil_search_context_t* ctx = dsmil_search_create();
const char* members[] = {"telemetry.csv", "events.json"};
int ok = dsmil_search_batch_tar_zst(ctx, "data.tar.zst", members, 2, keys, num_keys, results);
dsmil_search_destroy(ctx);
```

---

## Backend Selection

```mermaid
flowchart TD
    Start[Batch Query] --> CheckSize{Array Size?}
    CheckSize -->|Small| Scalar[Scalar C]
    CheckSize -->|Large| CheckSIMD{CPU Features?}

    CheckSIMD -->|AVX-512| AVX5[AVX-512 C]
    CheckSIMD -->|AVX2| AVX2[AVX2 C]
    CheckSIMD -->|None| Scalar

    CheckSIMD -->|OpenMP + Large| OMP[OpenMP Parallel]
    CheckSIMD -->|Fortran Avail| FOR[Fortran Batch]

    Scalar --> Anchor[Anchor Table / Adaptive Learning]
    AVX5 --> Anchor
    AVX2 --> Anchor
    OMP --> Anchor
    FOR --> Anchor
```

---

## Feature Matrix

| Feature | C Scalar | C AVX2 | C AVX-512 | OpenMP | Fortran | tar.zst |
|---------|----------|--------|-----------|--------|---------|---------|
| Single search | ✅ | ✅ | ✅ | — | — | — |
| Batch search | ✅ | ✅ | ✅ | ✅ | ✅ | — |
| Auto backend | ✅ | ✅ | ✅ | ✅ | ✅ | — |
| Anchor learning | ✅ | ✅ | ✅ | ✅ | ✅ | — |
| Archive ingestion | — | — | — | — | — | ✅ |

---

## Benchmark Example

```bash
$ ./benchmark.sh
Running KEYSTONE benchmark suite...
Running profile: 100K_dense
Running profile: 1M_dense
Running profile: 1M_jitter
Running profile: 10M_dense
Generating benchmark_comparison.png...
Saved benchmark chart to: benchmark_comparison.png
```

Sample output chart (`benchmark_comparison.png`):
- **Left**: Average latency per backend (ns/op, log scale)
- **Right**: Average speedup vs binary search

---

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `KEYSTONE_ENABLE_AVX2` | `auto` | Force-enable/disable AVX2 |
| `KEYSTONE_ENABLE_AVX512` | `0` | Force-enable AVX-512 |
| `KEYSTONE_ENABLE_OPENMP` | `1` | Enable OpenMP parallelism |
| `KEYSTONE_ENABLE_FORTRAN` | `1` | Build/link Fortran backend |
| `KEYSTONE_ENABLE_TAR_ZST` | `auto` | Enable tar.zst ingestion |
| `KEYSTONE_FORCE_SCALAR` | `0` | Force scalar-only build |

---

## Requirements

- **Compiler**: GCC or Clang with C11 support
- **Optional**: `gfortran` (Fortran backend)
- **Optional**: `libarchive` + `libzstd` (tar.zst support)
- **Optional**: `pkg-config` (tar.zst auto-detection)
- **Python**: `numpy`, `matplotlib` (benchmark chart generation)

---

## License

**AGPL-3.0-or-later. This is strong copyleft. See [LICENSE](LICENSE) before any commercial use.**

This project is published as a technical showcase and for home deployment if you so wish, bear me in mind if you want a world class database driving your fancy new framework. Failure to comply will be treated as copyright infringement and pursued to the full extent of the law.


---

## Repository

[https://github.com/SWORDIntel/KEYSTONE](https://github.com/SWORDIntel/KEYSTONE)
