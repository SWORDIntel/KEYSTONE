# KEYSTONE Benchmark Directory

This directory contains the automated performance benchmarking suites for KEYSTONE, measuring throughput, latency distributions (p50, p95, p99), memory footprints (RSS), and silicon acceleration paths across algorithmic search, indexing, and federation subsystems.

---

## Benchmark Suites

| Binary / Source | Subsystem Measured | Primary Metrics | Documentation |
| :--- | :--- | :--- | :--- |
| **`bench_federation`**<br>([`bench_federation.c`](bench_federation.c)) | **CITADEL Federation Intelligence** (Phases 0–7)<br>Wire ingest, exact identity, temporal index, topology hybrid planner, streaming telemetry, incident similarity, federated query coordinator, RAG retrieval | Throughput, p50/p95/p99 latency, zero-copy dedup ring, multi-node merge | [FEDERATION_BENCHMARK.md](FEDERATION_BENCHMARK.md) |
| **`trigram_benchmark`**<br>([`trigram_benchmark.c`](trigram_benchmark.c)) | **Vectorized 24-bit Trigram Text Index**<br>SSE4.2/AVX2 multi-threaded ingestion and candidate filtering | Ingestion MB/s, query speedups (77x–1046x vs brute-force), 99.98% candidate rejection | [../docs/TRIGRAM_BENCHMARK.md](../docs/TRIGRAM_BENCHMARK.md) |
| **`bench_memory_ramp`**<br>([`bench_memory_ramp.c`](bench_memory_ramp.c)) | **Memory Capacity & Cold-Cache Sweeps**<br>HugePage allocation, RSS, minor/major page faults, L1/L2/L3 eviction sweeps | Page fault counts, cold vs warm latency penalties (1.1x–2.7x) | [../docs/BENCHMARK_RESULTS.md](../docs/BENCHMARK_RESULTS.md) |
| **`dsmil_benchmark`**<br>([`dsmil_benchmark.c`](dsmil_benchmark.c)) | **SIMD Search & Auto-Backend Dispatch**<br>Scalar, AVX2, AVX-512, OpenMP, Fortran batch search paths | Lookups/second across dense, sparse, strided, and random workloads | [../docs/BENCHMARK_RESULTS.md](../docs/BENCHMARK_RESULTS.md) |
| **`performance_proof`**<br>([`performance_proof.c`](performance_proof.c)) | **End-to-End Search Proof**<br>Comparative verification across scalar and vectorized kernels | Correctness cross-check, latency bounds | [../docs/BENCHMARK_RESULTS.md](../docs/BENCHMARK_RESULTS.md) |

---

## Building and Running

### Build All Benchmarks
```bash
make benchmarks
```

### Run Federation Intelligence Benchmarks
```bash
make benchmarks/bench_federation
./benchmarks/bench_federation
```

### Run Trigram Index Benchmarks
```bash
make benchmarks/trigram_benchmark
./benchmarks/trigram_benchmark
```

### Run Memory Ramp & Cold-Cache Benchmarks
```bash
make benchmarks/bench_memory_ramp
./benchmarks/bench_memory_ramp
```
