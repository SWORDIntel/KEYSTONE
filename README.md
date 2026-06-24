<p align="center">
  <img src="docs/Logo.png" alt="KEYSTONE logo" width="720">
</p>

### KEYSTONE helps large databases find the right record fast — using adaptive search, CPU acceleration, and repeatable indexing logic.

[![C](https://img.shields.io/badge/C-11-blue.svg)](https://en.wikipedia.org/wiki/C11_(C_standard_revision))
[![Fortran](https://img.shields.io/badge/Fortran-90%2B-purple.svg)](https://en.wikipedia.org/wiki/Fortran)
[![Python](https://img.shields.io/badge/Python-3-yellow.svg)](https://www.python.org/)
[![SIMD](https://img.shields.io/badge/SIMD-AVX2%20%7C%20AVX--512-black.svg)](https://en.wikipedia.org/wiki/Advanced_Vector_Extensions)
[![Parallel](https://img.shields.io/badge/Parallel-OpenMP-green.svg)](https://www.openmp.org/)
[![Archives](https://img.shields.io/badge/Ingestion-tar.zst-orange.svg)](https://facebook.github.io/zstd/)
[![Platform](https://img.shields.io/badge/Platform-Linux-success.svg)](https://www.kernel.org/)
[![License](https://img.shields.io/badge/License-AGPL--3.0-red.svg)](LICENSE)

**KEYSTONE** is a high-performance database indexing and lookup engine built for precise, repeatable retrieval over sorted 64-bit integer datasets.

It combines anchor-guided interpolation search, adaptive backend selection, SIMD-assisted local scans, optional OpenMP parallelism, an optional Fortran batch engine, and compressed archive ingestion into a single practical system for serious data infrastructure.

</div>

---

## Current Status

KEYSTONE is a working native C library and benchmark suite, not just a design note.

| Area | Status |
|---|---|
| Core sorted `int64_t` lookup | Implemented and tested |
| Anchor-guided interpolation search | Implemented and tested |
| Batch lookup | Implemented and tested |
| Runtime backend calibration and decision reporting | Implemented and tested |
| OpenMP batch path | Available when built with OpenMP |
| Fortran batch backend | Optional; enabled when requested or when the local toolchain supports it |
| `.tar.zst` archive search | Optional; enabled when `libarchive` and `libzstd` are available |
| AVX2 small-window scan | Implemented for native x86 builds with AVX2 |
| AVX-512 path | Build-gated and hardware-dependent; treat as experimental until measured on target silicon |

The default workflow is intentionally native: build on the machine, container image, or CPU family where the code will run, then measure there.

---

## Quick Start

```bash
make clean
make test
```

`make test` builds and runs the available test binaries for the current host. Optional components are included when their local dependencies are present.

For a scalar-only comparison build:

```bash
make clean
KEYSTONE_ENABLE_FORTRAN=0 KEYSTONE_ENABLE_TAR_ZST=0 KEYSTONE_FORCE_SCALAR=1 make test
```

For build-only verification:

```bash
make tests
```

For benchmarks:

```bash
make benchmarks
./benchmarks/dsmil_benchmark
./benchmarks/performance_proof
```

---

## Mission

Modern databases rarely fail because storage is unavailable. They fail because lookup paths drift, records fragment, indexes become inconsistent, and high-volume datasets become expensive to search reliably.

KEYSTONE addresses that layer directly.

It is designed for systems where record identity, index stability, and lookup speed matter at the same time. Instead of treating indexing as a side effect of storage, KEYSTONE treats precise indexed retrieval as the core primitive.

---

## What KEYSTONE Is

KEYSTONE is a functional indexing and lookup engine for sorted `int64_t` datasets. It sits close to the data layer, supporting fast search, batch lookup, telemetry processing, archive-aware ingestion, and performance validation.

It is not a decorative wrapper around a database. It is a low-level search component designed for practical integration into larger systems that need predictable record access.

| Capability | Purpose |
|---|---|
| **Anchor-guided interpolation search** | Reduces search work by using learned anchor points across sorted data. |
| **Single-key lookup** | Supports precise direct search for individual records. |
| **Batch lookup** | Handles high-volume query workloads efficiently. |
| **Runtime backend calibration** | Measures viable local backends on first use, caches the fastest decision, and reports the selected path. |
| **SIMD-assisted search windows** | Uses compiled AVX2 and build-gated AVX-512 scan paths where available. |
| **OpenMP parallelism** | Scales batch workloads across CPU threads when built with OpenMP. |
| **Fortran batch backend** | Provides an optional high-throughput backend for numerical batch processing. |
| **`.tar.zst` ingestion** | Supports compressed archive workflows and member offset indexing. |
| **Benchmark suite** | Measures latency, throughput, and backend behavior across dataset profiles. |
| **Test coverage** | Validates native search, auto backend selection, Fortran support, telemetry processing, archive handling, performance fixes, and enhanced lookup behavior. |

---

## Why It Matters

High-speed lookup is easy to claim and difficult to prove. Real systems need more than a fast happy path.

KEYSTONE is built around the problems that appear when datasets become large, query volume increases, and records must remain stable across repeated processing runs.

It is useful where the cost of bad indexing is operationally significant:

- stale or unstable lookup positions;
- slow batch search over large sorted datasets;
- duplicated lookup logic across services;
- poor visibility into backend performance;
- inconsistent ingestion from compressed archives;
- weak testability around search behavior;
- performance claims without reproducible benchmarks.

KEYSTONE provides a focused answer: a compact, benchmarkable, database-adjacent indexing engine with multiple execution backends and a clear performance model.

---

## Architecture

```mermaid
flowchart TB
    subgraph Search["Search Pipeline"]
        Q["Query Key / Query Batch"] --> Auto{"Runtime Backend Calibrator"}
        Auto -->|Single / Small Workloads| S["Scalar Anchor Search"]
        Auto -->|Sorted Query Batch| CB["Optimized C Batch"]
        Auto -->|Large Batch + OpenMP| MP["C OpenMP Batch"]
        Auto -->|Dense Numerical Batch| FT["Optional Fortran Batch"]
        S --> SIMD["SIMD-Assisted Local Scan<br/>AVX2 / AVX-512 when compiled and available"]
        S --> AT["Anchor Table"]
        CB --> AT
        SIMD --> AT
        MP --> AT
        FT --> AT
        AT --> R["Result Index"]
    end

    subgraph Ingestion["Archive and Telemetry Ingestion"]
        RAW["Raw CSV / JSON / Text"] --> TZST[".tar.zst Archive"]
        TZST --> TP["Telemetry Processor"]
        TP --> IDX["Member Offset Index"]
        IDX --> AT
    end

    subgraph Tuning["Runtime Tuning"]
        CPU["CPU Feature Detection"] --> Auto
        WL["Workload Shape"] --> Auto
        CAL["First-Use Local Timing Cache"] --> Auto
    end
```

---

## Backend Selection Model

```mermaid
flowchart TD
    Start["Incoming Lookup Workload"] --> Mode{"Single or Batch?"}
    Mode -->|Single| Scalar["Scalar Interpolation Search"]
    Mode -->|Batch| Size{"Dataset and Batch Size"}

    Size -->|Small / Low Overhead Preferred| Scalar
    Size -->|Sorted Batch| CB["Optimized C Merge-Walk Batch"]
    Size -->|Large / Repeated Queries| CPU{"Build + Workload Capabilities"}

    CPU -->|OpenMP Built + Enough Queries| OMP["C OpenMP Batch Execution"]
    CPU -->|Fortran Built + Dense Sorted Shape| FORTRAN["Fortran Batch Execution"]
    CPU -->|Otherwise| CB

    OMP --> Cal["Measured Local Calibration"]
    FORTRAN --> Cal
    CB --> Cal
    Scalar --> Anchor["Anchor Table / Adaptive Learning"]
    Cal --> Anchor
    Anchor --> Result["Stable Result Index"]
```

---

## Data Flow

```mermaid
flowchart LR
    Source["Source Dataset"] --> Normalize["Sorted int64_t Keyspace"]
    Normalize --> Anchor["Anchor-Guided Search Layer"]
    Anchor --> Backend["Selected Execution Backend"]
    Backend --> Index["Result Index"]
    Index --> Consumer["Database / Telemetry / Analysis Consumer"]

    Archive["Compressed Archive"] --> Member["Member Offset Index"]
    Member --> Anchor

    Bench["Benchmark Harness"] --> Backend
    Tests["Validation Suite"] --> Anchor
```

---

## System Profile

| Layer | Function |
|---|---|
| **Core search engine** | Performs anchor-guided interpolation search over sorted integer data. |
| **Adaptive backend layer** | Calibrates and routes workloads across scalar, optimized C batch, optional OpenMP, and optional Fortran paths. |
| **Anchor table** | Maintains learned search anchors to improve repeated lookup behavior. |
| **Batch processing layer** | Executes large query sets efficiently across available acceleration paths. |
| **Telemetry processor** | Supports structured ingestion and indexed lookup workflows for telemetry-style data. |
| **Archive interface** | Enables compressed `.tar.zst` workflows without treating archive handling as an afterthought. |
| **Benchmark harness** | Produces measurable performance evidence rather than vague speed claims. |
| **Validation suite** | Confirms core behavior, enhanced search, backend selection, archive support, and performance fixes. |

---

## Feature Matrix

| Feature | Scalar / Anchor C | Optimized C Batch | SIMD Local Scan | OpenMP Batch | Fortran Batch | `.tar.zst` |
|---|---:|---:|---:|---:|---:|---:|
| Single search | Yes | No | Yes, inside local windows | No | No | No |
| Batch search | Yes | Yes | Indirect | Optional | Optional | No |
| Auto backend calibration | Yes | Yes | Build/runtime detected | Optional measured candidate | Optional measured candidate | No |
| Anchor learning | Yes | No for merge-walk batch | Yes through scalar path | Per-thread clone path | No | No |
| Runtime tuning | Yes | Yes | Build/runtime gated | Build gated | Build gated | No |
| Archive ingestion | No | No | No | No | No | Yes |
| Member offset indexing | No | No | No | No | No | Yes |
| Benchmark validation | Yes | Yes | Partial / host-specific | Yes when built | Yes when built | Yes |
| Linux support | Yes | Yes | Host-dependent | Compiler/runtime-dependent | Toolchain-dependent | Dependency-dependent |

---

## Practical Use Cases

### Database Index Acceleration

KEYSTONE is suitable for systems that need fast lookup across large sorted integer keyspaces, especially where keys map into record offsets, entity identifiers, telemetry IDs, event IDs, or compressed member indexes.

### Canonical Record Retrieval

When records are normalized into stable integer identifiers, KEYSTONE can act as the lookup layer that keeps retrieval fast and repeatable.

### Telemetry and Event Search

Telemetry systems often generate large ordered datasets that must be searched repeatedly. KEYSTONE is designed for that access pattern: high-volume lookup, repeatable index resolution, and measurable backend performance.

### Archive-Aware Data Processing

Compressed archive workflows are common in telemetry, exports, backups, and evidence packages. KEYSTONE includes `.tar.zst` ingestion support so archive processing can remain close to the indexed lookup model.

### Benchmark-Driven Optimization

The project includes performance tooling intended to compare backend behavior across dense, large, and jittered dataset profiles. This makes it suitable as both functional software and a performance engineering showcase.

### Systems Programming Demonstration

KEYSTONE demonstrates practical low-level engineering across C11, optional Fortran, Python-based benchmark visualization, SIMD-aware execution, optional OpenMP parallelism, compressed archive ingestion, and structured validation.

---

## Performance Orientation

KEYSTONE is designed around a simple premise: search performance should be measurable, backend-aware, and reproducible.

The benchmark suite is intended to produce concrete latency and speedup comparisons across workload profiles, including dense datasets, larger million-scale datasets, jittered distributions, and backend-specific behavior.

| Performance Concern | KEYSTONE Response |
|---|---|
| **Small lookup overhead** | Scalar interpolation remains available where vector or parallel overhead would be wasteful. |
| **Large batch volume** | Optimized C batch, OpenMP, and optional Fortran paths provide acceleration options for heavier workloads. |
| **CPU variability** | Runtime feature detection plus first-use timing calibration supports backend selection based on resident hardware. |
| **Repeated lookup behavior** | Anchor learning helps reduce repeated search cost across sorted keyspaces. |
| **Benchmark credibility** | Dedicated benchmark outputs support reviewable performance claims. |

### Benchmark Posture

Performance numbers are meaningful only with the host CPU, compiler flags, feature toggles, dataset shape, query hit rate, and warmup policy attached. Treat checked-in benchmark reports as examples of measurement style, not universal guarantees.

For serious comparison, run the benchmark matrix on the target machine and keep the generated CSV/output with the exact build command.

---

## Repository Structure

```mermaid
flowchart LR
    Root["KEYSTONE Root"] --> SRC["src/"]
    Root --> INC["include/"]
    Root --> TEST["tests/"]
    Root --> BENCH["benchmarks/"]
    Root --> DOCS["docs/"]
    Root --> FORT["fortran/"]
    Root --> SCRIPTS["scripts/"]

    SRC --> Core["Core Search Engine"]
    SRC --> Wrapper["Integration Wrapper"]
    SRC --> Telemetry["Telemetry Processor"]
    SRC --> Archive["tar.zst Interface"]

    INC --> PublicAPI["Public Headers"]
    INC --> TelemetryAPI["Telemetry Headers"]
    INC --> WrapperAPI["Wrapper Headers"]

    TEST --> NativeTest["Native Core Tests"]
    TEST --> BackendTest["Backend Selection Tests"]
    TEST --> ArchiveTest["Archive Tests"]
    TEST --> PerfTest["Performance Tests"]

    BENCH --> Harness["Benchmark Harness"]
    BENCH --> Proof["Performance Proofing"]
    BENCH --> Charts["Visualization Tools"]

    FORT --> FBackend["Fortran Batch Backend"]
```

---

## Technical Showcase Value

KEYSTONE is intended to be read as serious software, not a throwaway benchmark experiment.

It showcases:

- low-level search engine design;
- deterministic lookup over sorted 64-bit keyspaces;
- adaptive backend dispatch;
- SIMD-aware systems programming;
- optional OpenMP batch parallelism;
- optional Fortran integration;
- compressed archive ingestion;
- telemetry-oriented data handling;
- benchmark-driven engineering;
- testable performance claims;
- practical database-adjacent design.

This makes it suitable for portfolio review, technical demonstration, internal tooling, research infrastructure, and performance-sensitive database support work.

---

## Intended Users

KEYSTONE is intended for technical users who care about lookup correctness, runtime behavior, and database-adjacent performance.

| User Type | Why It Fits |
|---|---|
| **Database engineers** | Useful for fast lookup layers and stable integer keyspaces. |
| **Systems programmers** | Demonstrates C11, SIMD, OpenMP, Fortran integration, and archive handling. |
| **Security researchers** | Useful for telemetry, indicator stores, evidence indexes, and high-volume lookup datasets. |
| **Data engineers** | Supports repeatable indexing before downstream processing or enrichment. |
| **Performance engineers** | Provides benchmarkable backend behavior across workload shapes. |
| **Research teams** | Works as a compact foundation for indexed retrieval experiments. |

---

## Quality Model

| Goal | Standard |
|---|---|
| **Correctness** | Lookup behavior should remain predictable across supported backends. |
| **Repeatability** | The same sorted dataset and key should resolve consistently. |
| **Performance visibility** | Backend performance should be measurable rather than assumed. |
| **Backend flexibility** | Scalar, SIMD, parallel, and Fortran paths should serve different workload profiles. |
| **Archive practicality** | Compressed ingestion should integrate with the lookup model rather than exist as a detached helper. |
| **Operational usefulness** | The system should be practical for real database, telemetry, and analysis workflows. |

---

## Requirements

| Area | Requirement |
|---|---|
| **Operating system** | Linux |
| **Primary compiler** | GCC or Clang with C11 support |
| **Optional numerical backend** | Fortran 90+ capable compiler |
| **Optional archive support** | `libarchive` and `libzstd` |
| **Optional build detection** | `pkg-config` |
| **Benchmark visualization** | Python 3 with numerical and plotting support |
| **Parallel acceleration** | OpenMP-capable compiler/runtime |
| **Vector acceleration** | AVX2 or AVX-512 capable CPU where available |

---

## Native Tuning Model

KEYSTONE is intentionally built as a native, silicon-tuned component. The
default Makefile uses `-O3 -march=native` and enables resident CPU paths such as
AVX2, optional AVX-512, OpenMP, Fortran, and `.tar.zst` support when the local
toolchain and libraries allow it.

That means the preferred deployment model is to build KEYSTONE on the machine,
container image, or target CPU family where it will run. It is not trying to
produce one lowest-common-denominator binary for every host. For reproducible
comparisons, pin the build flags and optional feature switches explicitly:

```bash
make clean
KEYSTONE_ENABLE_FORTRAN=0 KEYSTONE_ENABLE_TAR_ZST=0 KEYSTONE_FORCE_SCALAR=1 make test
```

For the normal native path:

```bash
make test
```

To build without executing tests:

```bash
make tests
```

This native posture is deliberate. KEYSTONE favors accurate local dispatch and reproducible local measurement over a single portable binary that hides the silicon-specific behavior.

---

## Security and Data Handling

KEYSTONE is designed for datasets that may be operationally sensitive. Treat inputs, benchmark outputs, generated indexes, and archive contents according to the sensitivity of the source material.

Recommended handling posture:

- do not commit private datasets or generated operational indexes;
- keep production database credentials outside the repository;
- validate archive contents before processing untrusted inputs;
- separate benchmark datasets from real operational data;
- preserve audit trails where indexed records support legal, investigative, or high-trust decisions;
- treat indexing errors as data integrity failures, not cosmetic defects.

---

## What KEYSTONE Is Not

KEYSTONE is not a general spreadsheet sorter, dashboard framework, ORM, database replacement, or broad ETL platform.

It is a focused indexing and lookup component for sorted integer datasets. Its strength is precision, backend-aware performance, and suitability for integration into larger database and telemetry systems.

---

## Roadmap

Planned development areas:

- stronger benchmark reporting;
- richer backend comparison output;
- expanded archive ingestion profiles;
- improved telemetry processor documentation;
- additional workload profiles for sparse and skewed datasets;
- clearer integration guidance for database-backed systems;
- richer validation around index stability;
- packaging improvements;
- expanded platform notes;
- deeper documentation for anchor behavior and tuning.

---

## License

**AGPL-3.0-or-later. This is strong copyleft. See [LICENSE](LICENSE) before use, modification, redistribution, hosting, or derivative work.**

Network use, redistribution, modification, and derivative use carry obligations. Do not treat this repository as permissive code.

If your intended use is proprietary, closed-source, commercial, hosted, or otherwise incompatible with AGPL compliance, obtain written permission or a separate license from the repository owner first.

KEYSTONE does not include covert license telemetry or phone-home enforcement. Compliance is enforced through the AGPL license terms, copyright ownership, visible notices, and separate commercial licensing where appropriate.

Unlicensed use outside the terms of the repository license is not authorized. Respect the license, attribute properly, and do not repackage the work as your own.

For commercial or proprietary licensing discussions, contact the repository owner.

---

<div align="center">

**KEYSTONE**  
Precision indexing. Adaptive lookup. Database discipline.

</div>
