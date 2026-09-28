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
  - High-throughput deduplication hash ring (measured rates in [`benchmarks/FEDERATION_BENCHMARK.md`](../benchmarks/FEDERATION_BENCHMARK.md); figures are host- and load-dependent).
  - Instant tombstone registry masking, fencing epoch protection, and atomic double-buffered checkpointing.
- **Phase 1: Infrastructure Exact Identity & Monotonic Temporal Indexing** ([`include/keystone_exact_index.h`](../include/keystone_exact_index.h), [`include/keystone_temporal.h`](../include/keystone_temporal.h)):
  - $O(1)$ open-addressing exact identity index (see [`benchmarks/FEDERATION_BENCHMARK.md`](../benchmarks/FEDERATION_BENCHMARK.md) for measured lookup rates).
  - Monotonic temporal index with 80-byte entries (HLC + event/object UUID + type/tenant/classification/flags + generation + payload offset); range, object-scoped, reverse-scan, and bucket-aggregation queries (see benchmark doc for measured rates).
  - 2026-09-28: fixed a production heap overflow in `keystone_temporal_index_load` (claimed capacity exceeded the allocated array on resume) and hardened the loader against hostile persisted files — exact file-size bound, checked arithmetic, reserved-word and CRC validation. See [`docs/plans/KEYSTONE_FEDERATION_ALIGNMENT_AUDIT_2026-09-28.md`](plans/KEYSTONE_FEDERATION_ALIGNMENT_AUDIT_2026-09-28.md).
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

All **18 / 18 test suites** built by `make check` pass (verified 2026-09-28; see the live-verification note below for the local-vs-production distinction):

| Test Suite Binary | Focus Area | Status |
|---|---|---|
| `bin/test_enhanced` | DSMIL integration & error handling | PASS |
| `bin/test_auto_backend` | Auto-backend calibration & fallback | PASS |
| `bin/test_telemetry_processor_perf` | Telemetry processor | PASS |
| `bin/test_performance_fix` | Performance regression guards | PASS |
| `bin/test_core_native` | Core scalar & SIMD search | PASS |
| `bin/test_trigram_index` | Trigram engine correctness | PASS |
| `bin/test_memory_ramp` | Huge pages & memory capacity ramp | PASS |
| `bin/test_fortran_backend` | Fortran interop & ABI | PASS |
| `bin/test_fortran_workloads` | Fortran multi-workload benchmarking | PASS |
| `bin/test_tar_zst` | Compressed archive streaming (incl. corrupt members) | PASS |
| `bin/test_keystone_fabric` | QIHSE node capability bus frames | PASS |
| `bin/test_cuda_backend` | CUDA & AMX silicon detection + fallback contracts | PASS |
| `bin/test_federation_envelope` | Phase 0 wire envelope, dedup ring, checkpoints | PASS |
| `bin/test_exact_temporal_index` | Phase 1 exact & temporal indexes + persistence corruption | PASS |
| `bin/test_keystoned_service` | Phase 2 daemon, IPC & security context denial | PASS |
| `bin/test_topology_hybrid_planner` | Phase 3 topology cache & hybrid planner | PASS |
| `bin/test_telemetry_incident_engine` | Phase 4 & 5 telemetry & silicon dispatch | PASS |
| `bin/test_federated_query_rag` | Phase 6 & 7 federated coordinator & RAG | PASS |

Known caveat: the `ns_per_query < 500` assertion inside `test_exact_temporal_index` is a timing
gate that is borderline on loaded hosts (it has been observed to flake at ~500 ns on a busy PVE
container while passing at 477–498 ns on quiet runs). It is a performance gate, not a
correctness failure.

---

## Live-Verification Status (2026-09-28)

Local test/benchmark success is not production verification. Current evidence tiers:

- **Live-verified (fleet):** one consumer path — the R6b audit-journal feed
  (`../INFRA/parrot-sabot/scripts/keystone_journal_feed.py`), ctypes over `libkeystone`
  federation/exact/temporal APIs, cursor resume + spool verification against the production
  cluster (2026-09-26; re-verified read-only 2026-09-28, 3/4 items consistent, 1 item showing
  source-side same-generation content drift — see the audit).
- **Implemented + locally tested:** Phases 0–7 engines and their test binaries (above),
  plus a clean ASan/UBSan and valgrind matrix over the C suites — wired as first-class
  `make asan` / `make valgrind` targets (2026-09-28; the full-suite matrix also surfaced
  and fixed a latent serial-build crash in the trigram parallel builder and an
  uninitialized-UUID leak in the keystoned test).
- **Not verified / partial:** keystoned is not deployed anywhere; the federated query
  coordinator has no network transport; no KEYSTONE-side consumer of QIHSE's
  `KEYSTONE.FEED.NEXT` wire command exists yet; fuzzing and physical security partitioning are
  unbuilt. Full per-criterion verdicts:
  [`docs/plans/KEYSTONE_FEDERATION_ALIGNMENT_AUDIT_2026-09-28.md`](plans/KEYSTONE_FEDERATION_ALIGNMENT_AUDIT_2026-09-28.md).

---

## Future Research & Horizons

1. **FPGA & CXL Near-Memory Acceleration**:
   - Evaluate CXL 3.0 shared-memory fabrics for cross-node generation pointer sharing without network serialization.
2. **Post-Quantum Cryptographic Signatures for Model Governance**:
   - Upgrade model weight manifests to support ML-DSA (Dilithium) signatures alongside SHA-256 digests.
3. **Adaptive Streaming Compression**:
   - Investigate stream dictionary training for continuous telemetry ingestion over bandwidth-constrained satellite or edge links.
