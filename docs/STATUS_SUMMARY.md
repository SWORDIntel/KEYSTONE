<!--
  SPDX-License-Identifier: AGPL-3.0-or-later
  Copyright (C) 2026 SWORDIntel. All rights reserved.
-->

# KEYSTONE Status Summary

Current engineering status across core search, memory optimization, vectorized trigram indexing, vector similarity, downstream fabric integration, and the **CITADEL Federation Intelligence Upgrade (Phases 0–7)**.

---

## Done

### 1. Core Search & SIMD
- **Scalar C Reference Path**: Deterministic baseline path with 100% cross-validation against accelerated backends.
- **Runtime CPU Feature Detection**: Probes AVX2, AVX-512 (F, DQ, BW), Intel AMX (TILE, INT8, BF16), ARM NEON, and SVE.
- **AVX2 Local Scan Path**: Vectorized small-window search for x86 AVX2 platforms.
- **SSE4.2 Integer Vectorization**: 128-bit SIMD path (`_mm_cmpeq_epi64`), 2x unrolled for legacy x86 CPUs.
- **Software Prefetching**: Configured L1 (`_MM_HINT_T0`) and L2 (`_MM_HINT_T1`) prefetch pipelines for medium/large search arrays.

### 2. Memory & Concurrency
- **Transparent Huge-Page Hints**: `keystone_optimize_array_memory()` using `madvise(MADV_HUGEPAGE)` for arrays >1MB; zero major page faults up to 128M rows.
- **Lock-Free Calibration Cache**: Reader-writer lock (`pthread_rwlock_t`) on auto-backend calibration cache enabling high-concurrency queries without mutex contention.
- **Zero-Copy Radix Sort**: 8-pass Least Significant Digit (LSD) radix sort (`src/dsmil_hash_indexer.c`) with double-buffered pointer swapping, eliminating 32 full-array `memcpy` operations.

### 3. Batch Search & Runtime Auto-Selection
- **`keystone_search_batch_auto()`**: First-use dynamic benchmarking across viable scalar, optimized C, OpenMP, and optional Fortran candidates.
- **Calibration Cache**: Keyed on CPU features, array size bucket, query count bucket, thread count, query shape, 25%-granular hit-rate buckets, gap buckets, and detected stride.
- **Decision Provenance**: Public exposure of decision source (fast path, measured, cache, static fallback), workload profile (`hit_rate_pct`, `avg_gap`, `detected_stride`), and measured median/p95 latency.
- **Testing Switches**: Validated fallback policies via `KEYSTONE_DISABLE_CALIBRATION_CACHE` and `KEYSTONE_FORCE_CALIBRATION_FALLBACK`.

### 4. Vector Similarity Engine (`vector_engine/`)
- **Arbitrary & 384-Dim Float32 Embeddings**: Cosine, Euclidean (L2), and Dot product metrics.
- **Locality-Sensitive Hashing (LSH)**: Coarse index with multiprobe search, 4-way unrolled FMA projection, 64-bit quick bloom filter candidate deduplication, and $O(N \log N)$ `qsort` bucket finalization.
- **Multi-Tier Silicon Distance Kernels**: Scalar, SSE4.2, AVX, AVX2, AVX-512, ARM NEON, Myriad X VPU, and CUDA GPU with graceful dynamic fallback.
- **Zero-Heap Query Fast Paths**: Stack-allocated scratchpads for query normalization (up to 1,024 dims) and candidate reranking (up to 512 candidates).

### 5. Streaming Archive Ingestion (`src/keystone_tar_zst.c`)
- **Zero-Disk Inflation**: Streaming `.tar.zst` decompression and parsing directly in memory.
- **Persistent Sidecar Indices**: `<archive>.idx.json` with compact Bloom filters for sub-millisecond archive startup and $O(1)$ negative rejection.
- **Pipelined Ring Buffers**: Dual-threaded producer/consumer ring buffer overlapping I/O and decompression.
- **Multi-Archive Batch Pools**: Parallel evaluation of partitioned archives with OpenMP candidate pruning.

### 6. Vectorized Trigram Content Indexing (`bin/tgrep` & `src/keystone_trigram.c`)
- **24-Bit Direct Directory**: Flat 64 MiB directory (`KEYSTONE_TRIGRAM_OPT_DIRECT_DIRECTORY`) providing $O(1)$ zero-probe trigram lookups without hash collisions.
- **SIMD Intersection & Galloping Search**: Adaptive list intersection combining blocked AVX2 comparisons with monotonic galloping search (`ks_lower_bound_gallop_u32`).
- **Dense Posting Bitmaps**: High-frequency trigrams converted to 64-bit word bitmaps with $O(1)$ bit test and bitwise AND.
- **Multi-Threaded Construction**: Parallel build (`keystone_trigram_index_build_parallel`) achieving 6.7x speedup on 1GB corpora (46 MB/s, 957M postings).
- **Standalone `bin/tgrep` CLI**: High-performance grep replacement with persistent index caching (`-I` / `.tgrep.idx`), case folding, and SIMD substring verification.

### 7. Downstream Compute Fabric Integration
- **Parrot-Sabot**: Integrated `TrigramIndex` into `ArtifactSpooler` for `/search-artifacts` sub-millisecond log triage.
- **QIHSE Node Capability Frame**: `include/keystone_fabric.h` exports hardware ISA capabilities, accelerator status, and live resource stats to the QIHSE cluster bus via 66-byte datagrams (`0x51424E53` magic).

### 8. CITADEL Federation Intelligence Upgrade (Phases 0–7 Complete)
- **Phase 0: Federation Wire Envelope & Dual Ingestion** ([`include/keystone_federation.h`](../include/keystone_federation.h), [`src/federation/keystone_federation_ingest.c`](../src/federation/keystone_federation_ingest.c)):
  - Packed 124-byte wire envelope (`KEYSTONE_FEDERATION_ENVELOPE_MAGIC = 0x4B534645`), IEEE 802.3 CRC32 header and payload checksums.
  - 128-bit UUID primitives (`keystone_uuid_t`) and monotonic Hybrid Logical Clock (`keystone_hlc_t`).
  - High-throughput deduplication hash ring benchmarked at **9.3+ million events/sec** (107 ns/event).
  - Instant tombstone registry masking, fencing epoch protection, and atomic double-buffered checkpointing.
- **Phase 1: Infrastructure Exact Identity & Monotonic Temporal Indexing** ([`include/keystone_exact_index.h`](../include/keystone_exact_index.h), [`include/keystone_temporal.h`](../include/keystone_temporal.h)):
  - $O(1)$ open-addressing exact identity index benchmarked at **4.6+ million queries/sec** (217 ns/query).
  - 64-byte cache-line aligned monotonic temporal index benchmarked at **3.7+ million range queries/sec** (269 ns/query).
  - Object-scoped timeline queries, time-bucket histogram aggregations, and out-of-order arrival stabilization.
- **Phase 2: Security-Aware Native Service Mode (`keystoned`)** ([`include/keystoned.h`](../include/keystoned.h), `bin/keystoned`, [`src/service/`](../src/service/)):
  - Unprivileged service daemon operating over restricted `0700` Unix domain sockets (`/run/keystone/keystoned.sock`).
  - Multi-client poll concurrency and atomic reader-writer generation publication (`keystoned_server_publish_generation`).
  - Multi-level security context partitioning (`clearance_level`, `compartment_mask`, `tenant_id`) with zero metadata leakage (`KEYSTONED_STATUS_DENIED`).
- **Phase 3: Topology Graph Cache & Two-Tier Hybrid Query Planner** ([`include/keystone_topology.h`](../include/keystone_topology.h), [`include/keystone_hybrid.h`](../include/keystone_hybrid.h)):
  - In-memory graph adjacency cache (`RUNS_ON`, `ATTACHED_TO`, `ROUTES_THROUGH`, `DEPENDS_ON`, `REPLICATED_TO`, `SHARES_FAILURE_DOMAIN`).
  - Neighborhood expansion, failure domain clustering, and dependency chain blast radius tracing.
  - Two-tier hybrid query planner: Tier 1 boolean constraint pruning (security, CPU ISA flags, RAM, anti-affinity) and Tier 2 weighted soft ranking (RAM, CPU, thermals, NUMA).
  - Explainable recommendation bundles (`keystone_recommendation_t`, `keystone_explain_t`).
- **Phase 4 & 5: Streaming Telemetry & Silicon Incident Similarity** ([`include/keystone_telemetry.h`](../include/keystone_telemetry.h), [`include/keystone_incident.h`](../include/keystone_incident.h)):
  - Circular sample buffers, online Welford statistics, and rolling multi-tier feature windows (1m, 5m, 15m, 1h, 24h).
  - Deterministic multi-stage anomaly engine (static warning/critical limits, $|z| \ge 3.0$ statistical outliers with zero-variance protection, linear regression slope/trend).
  - 64-dimensional normalized incident vector embeddings with multi-tier silicon dispatch: NVIDIA CUDA GPU, Intel Sapphire Rapids AMX (`_tile_dpbssd`), AVX-512, AVX2+FMA, and Scalar CPU reference fallback.
  - Historical incident similarity search emitting advisory mitigations.
- **Phase 6 & 7: Federated Distributed Query & AI/RAG Context Retrieval** ([`include/keystone_federated_query.h`](../include/keystone_federated_query.h), [`include/keystone_rag.h`](../include/keystone_rag.h)):
  - Federated multi-node coordinator, node health and latency tracking, and deterministic conflict resolution (Epoch $\succ$ Generation $\succ$ HLC).
  - Multi-way monotonic timeline merge and distributed top-K similarity search aggregation with partial-result degradation flags (`keystone_partial_status_t`).
  - Constrained RAG context pack extraction (`keystone_context_pack_t`) with explicit grounding citations (`keystone_citation_t`).
  - Cryptographic model governance registry (`keystone_model_manifest_t`) validating task capabilities and SHA-256 weight hash integrity.

---

## Test Suite Status

All **19 / 19 test suites** pass 100% green under `make check`:

| Test Suite Binary | Focus Area | Status |
|---|---|---|
| `bin/test_keystone` | Core scalar & SIMD search | PASS |
| `bin/test_keystone_calibration_cache` | Calibration cache & concurrency | PASS |
| `bin/test_keystone_provenance` | Workload shape profiling & provenance | PASS |
| `bin/test_keystone_fortran` | Fortran interop & ABI | PASS |
| `bin/test_fortran_workloads` | Fortran multi-workload benchmarking | PASS |
| `bin/test_memory_ramp` | Huge pages & memory capacity ramp | PASS |
| `bin/test_cuda_backend` | CUDA & AMX silicon detection | PASS |
| `bin/test_keystone_tar_zst` | Compressed archive streaming | PASS |
| `bin/test_trigram_index` | Trigram engine correctness | PASS |
| `bin/test_trigram_sol` | Sol trigram optimization passes | PASS |
| `bin/test_trigram_parallel` | Multi-threaded parallel trigram build | PASS |
| `bin/test_keystone_fabric` | QIHSE node capability bus frames | PASS |
| `bin/test_keystone_qihse_integration` | QIHSE cluster bus integration | PASS |
| `bin/test_federation_envelope` | Phase 0 wire envelope & dedup ring | PASS |
| `bin/test_exact_temporal_index` | Phase 1 exact & temporal indexes | PASS |
| `bin/test_keystoned_service` | Phase 2 daemon, IPC & security context | PASS |
| `bin/test_topology_hybrid_planner` | Phase 3 topology cache & hybrid planner | PASS |
| `bin/test_telemetry_incident_engine` | Phase 4 & 5 telemetry & silicon dispatch | PASS |
| `bin/test_federated_query_rag` | Phase 6 & 7 federated coordinator & RAG | PASS |

---

## Future Research & Horizons

1. **FPGA & CXL Near-Memory Acceleration**:
   - Evaluate CXL 3.0 shared-memory fabrics for cross-node generation pointer sharing without network serialization.
2. **Post-Quantum Cryptographic Signatures for Model Governance**:
   - Upgrade model weight manifests to support ML-DSA (Dilithium) signatures alongside SHA-256 digests.
3. **Adaptive Streaming Compression**:
   - Investigate stream dictionary training for continuous telemetry ingestion over bandwidth-constrained satellite or edge links.
