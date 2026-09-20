# KEYSTONE: Vectorized Search, Trigram Indexing & Compute Fabric Roadmap

A high-performance C11/SIMD algorithmic search and indexing library featuring runtime hardware feature dispatch (AVX2, AVX-512, AMX, NEON), vectorized trigram text indexing, 384-dim float32 LSH vector similarity search, streaming `.tar.zst` archive triage, and auto-calibrated execution backends.

Independent repository (`SWORDIntel/KEYSTONE`), tracked as a core dependency in `SWORDIntel/INFRA` and utilized by `SWORDIntel/parrot-sabot` and `SWORDIntel/QIHSE`.

---

## 1. Completed Capabilities (Status: Implemented)

### Core Vectorized Search & Memory
- [x] **Scalar C Reference Path**: Fully verified fallback path with deterministic cross-checking.
- [x] **Runtime CPU Feature Detection**: Real-time detection of AVX2, AVX-512F, AVX-512BW, AMX-TILE, ARM NEON, and SVE.
- [x] **AVX2 Local Scan Path**: Vectorized small-window search for x86 AVX2 platforms.
- [x] **Transparent Huge-Page Hints**: `keystone_optimize_array_memory()` using `madvise(MADV_HUGEPAGE)` for arrays >1MB, resulting in zero major page faults up to 128M rows.
- [x] **Software Prefetching**: Configured L1 (`_MM_HINT_T0`) and L2 (`_MM_HINT_T1`) prefetch pipelines for medium/large arrays.

### Auto-Backend Calibration & Concurrency
- [x] **Runtime Calibration Selector**: `keystone_search_batch_auto()` benchmarks candidate backends on first use using measured median/p95 timing.
- [x] **Lock-Free Calibration Cache**: Reader-writer lock (`pthread_rwlock_t`) on auto-backend calibration cache enabling high-concurrency queries without mutex contention.
- [x] **Workload Profiling**: Dynamic detection of query shapes (`dense sorted`, `sparse sorted`, `strided`, `random`), hit rates, and gaps.
- [x] **Zero-Copy LSD Radix Sort**: Double-buffered pointer swapping in `dsmil_hash_indexer.c` replacing 32 full-array `memcpy` operations.

### Vector Similarity Engine (`vector_engine/`)
- [x] **Arbitrary & 384-Dim Float32 Embeddings**: Cosine, Euclidean (L2), and Dot product metrics.
- [x] **Locality Sensitive Hashing (LSH)**: Coarse index with multiprobe search, 4-way unrolled FMA projection, 64-bit quick bloom filter deduplication, and $O(N \log N)$ bucket sorting.
- [x] **Multi-Tier Distance Kernels**: Scalar, SSE4.2, AVX, AVX2, AVX-512, ARM NEON, Myriad X VPU, and CUDA with dynamic fallback.
- [x] **Zero-Heap Query Scratchpads**: Fixed stack/thread scratchpads for cosine normalization and candidate reranking.

### Streaming Archive Ingestion (`src/keystone_tar_zst.c`)
- [x] **Zero-Disk Inflation**: Streaming `.tar.zst` decompression and parsing directly in memory.
- [x] **Persistent Sidecar Indices**: `<archive>.idx.json` with compact Bloom filters for sub-millisecond archive startup and $O(1)$ negative rejection.
- [x] **Pipelined Ring Buffers**: Dual-threaded producer/consumer ring buffer overlapping I/O and decompression.

### Trigram Content Indexing V1 (`src/keystone_trigram.c`)
- [x] **24-Bit Packed Trigram Engine**: Sub-linear text indexing and candidate rejection delivering 100x+ triage speedups over raw scans.
- [x] **Unified C Interface**: Single-include integration in `<keystone.h>`.
- [x] **Case-Insensitive Ingestion**: Fast lowercase folding and case-insensitive substring verification.
- [x] **Zero-Rebuild Persistence**: Binary serialization and deserialization (`keystone_trigram_index_save`/`load`).
- [x] **Direct `.tar.zst` Stream Indexing**: Ingestion of compressed archive text streams via `keystone_tar_zst_index_trigram()`.
- [x] **Python SDK**: Complete `TrigramIndex` bindings with streaming ingestion and candidate iteration.

---

## 2. Trigram Engine Optimization Roadmap (Sol / `gpt-5.6-sol` Architecture)

Formulated in architectural review with frontier model Sol (`gpt-5.6-sol`). See complete review in [`docs/sol_trigram_review.md`](docs/sol_trigram_review.md).

### Phase 1: Immediate Wins (Hot-Path Corrections & SIMD Intersection) — COMPLETED
- [x] **Fix Case-Insensitive Iterator Folding**:
  - Corrected bug where candidate iterator (`keystone_trigram_candidates_begin`) called public `keystone_trigram_extract()`, which did not fold query characters while ingestion did.
- [x] **Monotonic Galloping Lower-Bound Search**:
  - Replaced scalar $O(N \cdot M \log K)$ `bsearch()` in candidate iterator with monotonic lower-bound galloping search (`ks_lower_bound_gallop_u32`), eliminating $O(N)$ restarts from position 0.
- [x] **Eliminate Per-Query `doc_count` Heap Allocation**:
  - Replaced `malloc(doc_count * sizeof(uint32_t))` on every query with stack-first bounded workspace buffers sized to $\min(\text{rarest\_count}, \text{max\_candidates})$.
  - Direct zero-allocation scan for queries $< 3$ bytes.
- [x] **Adaptive Sparse Posting Intersection**:
  - Implemented `ks_intersect_u32_adaptive`:
    - Skewed lists ($\ge 16\times$ ratio): Monotonic galloping search ($O(\min(N_A, N_B) \log(N_A / N_B))$).
    - Balanced lists: Blocked AVX2 merge using 8-way broadcasts and `_mm256_cmpeq_epi32`.
    - Single list fast-path: direct `memcpy`.
- [x] **Query Planner Rarity Pruning**:
  - Extract unique query trigrams, look up frequencies in index, and retain only the rarest 16 trigrams. Avoid evaluating non-discriminating lists when candidate count is already tiny.
- [x] **Free Ingestion-Only State at Finalize**:
  - Released 2MB document deduplication bitmap (`doc_seen`) and touched tracking buffer at `keystone_trigram_index_finalize()`.
- [x] **Standardized Fast ASCII Folding**:
  - Unified branchless ASCII folding (`fast_ascii_tolower`) across ingestion, query planning, and candidate verification, and pre-folded query needle in `bounded_memmem_ci`.

### Phase 2: Indexing & Storage Throughput
- [x] **Arena-Backed Posting List Construction**:
  - Replaced per-posting `malloc()` and `realloc()` copying with 2MB-block bump allocator chunk arena (`keystone_arena_t` and `keystone_posting_chunk_t`). Chunks scale geometrically up to 16,384 entries (64 KB) with zero pointer copying churn during ingestion. Unpacked into contiguous `flat_postings` and destroyed in a single pass at `finalize()`.
- [x] **Contiguous Flattened Index Layout**:
  - At `finalize()`, flatten all posting lists into a single contiguous memory pool (`flat_postings`), eliminating heap fragmentation and reducing index destruction to $O(1)$.
- [x] **Dense Posting Bitmap Conversion**:
  - Converted high-frequency trigrams ($\ge \text{doc\_count} / 32$, with $\ge 64$ docs) to 64-bit word bitmaps at `finalize()`. Intersect dense lists via $O(1)$ bit test and 64-bit word bitwise AND with `__builtin_ctzll`.
- [x] **Direct 24-Bit Descriptor Directory**:
  - Implemented `KEYSTONE_TRIGRAM_OPT_DIRECT_DIRECTORY`: flat 64 MiB directory (`16M * 4 bytes`) mapping every 24-bit trigram directly to its posting slice, achieving $O(1)$ zero-probe trigram lookups without hash collisions or probing loops. Integrated into C API and Python SDK `TrigramIndex(direct_directory=True)`.
- [x] **True Streaming Ingestion with 2-Byte Carry**:
  - Overhauled `keystone_trigram_stream` to carry the trailing 2 bytes across arbitrary buffer chunks with $O(1)$ buffer memory for external documents.

### Phase 3: Multi-Threaded Parallel Construction — COMPLETED
- [x] **Thread-Local Builders**:
  - Independent builder contexts per worker thread (`ks_local_builder_t`) with contiguous document ID ranges (`[begin, end)`).
  - Per-thread bump-allocated chunk arenas, dedicated 2MB dedup bitsets, and local Fibonacci hash tables eliminating all lock contention during document ingestion.
- [x] **Two-Pass Parallel Merge & OpenMP Construction**:
  - Implemented `keystone_trigram_index_build_parallel`: Pass 1 aggregates global trigram frequencies and pre-allocates contiguous `flat_postings`; Pass 2 parallel-copies worker postings across unique trigrams with `#pragma omp parallel for schedule(dynamic, 256)`, populates the direct 24-bit directory, and computes 64-bit dense bitmaps in parallel.
  - Benchmarked across 1MB, 10MB, 100MB, and 1GB corpora: achieved **6.7x build speedup** on 1GB corpus (22.31s vs 148.62s single-threaded, 46 MB/s indexing throughput, 957.4 million postings).
  - Full Python SDK integration via `TrigramIndex.build_parallel(docs, thread_count=0)`.

### Phase 4: Standalone High-Performance CLI (`bin/tgrep`) — COMPLETED
- [x] **Pure C11 CLI Search Executable**:
  - Implemented standalone `bin/tgrep` built directly on Keystone's 24-bit direct-directory trigram indexing engine with multi-threaded parallel ingestion (`keystone_trigram_index_build_parallel`).
  - Native support for standard search flags: `-i` (case-insensitive), `-n` / `-N` (line numbering), `-l` (files with matches), `-c` (count per file), `-j <N>` (worker threads, default 1/2 host cores), and `--color`.
- [x] **Persistent Index Acceleration (`-I` & `--build-index`)**:
  - Direct binary index loading and caching (`.tgrep.idx`) achieving sub-millisecond query startup and $O(1)$ trigram lookup times (0.004 ms).
  - Pre-build index support via `--build-index <file>` for instant offline triage over massive repositories.
- [x] **Fast Recursive Traversal & SIMD Verification**:
  - High-throughput directory walker skipping hidden folders (`.git`, `.cache`, `.pytest_cache`), build artifacts (`node_modules`, `target`, `build`), and binary files via 1024-byte probe.
  - SSE4.2 / AVX2 vector substring verification with ANSI terminal highlighting.
- [x] **Document Inspection C & Python APIs**:
  - Added `keystone_trigram_index_get_document()` to C API and `TrigramIndex.get_document()` to Python SDK.

---

## 3. Backlog & Hardware Scaling

### Fortran & Scientific Search
- [x] **Expanded Workload Benchmarking**:
  - Evaluated Fortran `keystone_batch_search_i64` and `keystone_search_batch_fortran` across sparse, strided, and mixed hit/miss distributions in `tests/test_fortran_workloads.c`.
  - Benchmarked up to 5.95x speedups on sparse sorted queries and up to 2.92x on mixed hit/miss workloads with 100% scalar agreement.
- [x] **Auto-Route Widening**:
  - Expanded auto-calibration candidate routing in `keystone_calibrate_auto_backend` and `keystone_search_batch_auto` beyond `KEYSTONE_QUERY_SHAPE_DENSE_SORTED` to evaluate `KEYSTONE_QUERY_SHAPE_SPARSE_SORTED` and `KEYSTONE_QUERY_SHAPE_STRIDED`.
  - Conditioned Fortran selection on demonstrating $\ge 10\%$ repeatable speedups (`current.median_ns_per_key < best.median_ns_per_key * 0.90`).

### Memory Ramp & Cold-Cache Verification
- [x] **RAM-Capacity Sweeps**:
  - Implemented `benchmarks/bench_memory_ramp.c` and `tests/test_memory_ramp.c` scaling across 1M-16M elements (test mode) and 5%, 10%, 25%, 40%, 60% of `MemAvailable` (benchmark mode).
  - Tracked `ru_maxrss`, `ru_minflt`, and `ru_majflt` via `getrusage(RUSAGE_SELF, &usage)` with zero major page faults and verified `keystone_optimize_array_memory()` (`madvise(MADV_HUGEPAGE)`).
- [x] **OS Cold-Cache Profiling**:
  - Formalized cache invalidation sweeps in `tests/test_memory_ramp.c` and `benchmarks/bench_memory_ramp.c` evicting L1/L2/L3 by dirtying and reading a 64MB+ working-set memory buffer.
  - Measures cold-start search latency vs warm-cache latency (demonstrating 1.1x - 2.7x cold penalty) without requiring root `/proc/sys/vm/drop_caches` privileges.

### Future Silicon
- [x] **AMX Matrix Acceleration**:
  - Implemented real-time CPUID detection for AMX-TILE and OS XCR0 state in `src/keystone.c` and `tests/test_cuda_backend.c`.
  - Implemented AMX tile configuration (`ldtilecfg`), matrix multiplication (`tdpbssd` / `tdpbf16ps`), and tile release primitives with verified deterministic scalar fallbacks across unsupported CPUs.
- [x] **Resident GPU/NPU Stress Testing**:
  - Implemented end-to-end hardware-in-the-loop CUDA batch binary search (`keystone_search_batch_cuda`, `keystone_search_batch_cuda_versioned`) and cache invalidation in `cuda/keystone_cuda.cu`.
  - Implemented vector engine GPU distance kernels (`keystone_cuda_dist_kernel` for cosine, L2, dot products) in `vector_engine/keystone_cuda.cu`.
  - Verified 100% agreement against scalar references and contract-compliant fallback paths (`tests/test_cuda_backend.c`).

---

## 4. Downstream Integrations

- [x] **Parrot-Sabot (`SWORDIntel/parrot-sabot`)**:
  - Integrated `TrigramIndex` into `ArtifactSpooler` (`sabot/artifacts.py`) with mtime caching for `/search-artifacts <query>` sub-millisecond log triage.
- [x] **QIHSE AI Compute Fabric (`SWORDIntel/QIHSE`)**:
  - Export Keystone engine capability frames (`NODE_CAP` frame 8u, 50-byte wire payload) to the QIHSE cluster bus.
  - Implemented `include/keystone_fabric.h`, `src/keystone_fabric.c` with hardware ISA tier mapping (AVX/AVX2/AVX-512/AMX), accelerator probes (NPU `/dev/accel` and VPU socket, GPU `/dev/dri` and `/dev/nvidiactl`), fresh memory/load stats, and UDP cluster bus emission (`0x51424E53` magic, 66-byte datagrams).
  - Integrated into Python SDK (`keystone.NodeCapability`, `keystone.probe_node_capability()`, `keystone.export_node_cap_frame()`, `keystone.broadcast_node_cap()`).
  - Wire compatibility verified via `tests/test_keystone_fabric.c` and QIHSE cluster bus integration test `tests/test_keystone_qihse_integration.c`.

---

## 5. CITADEL Federation Intelligence Upgrade

Governed by [`CITADEL/docs/architecture/KEYSTONE_FEDERATION_INTELLIGENCE_UPGRADE_BRIEF.md`](../CITADEL/docs/architecture/KEYSTONE_FEDERATION_INTELLIGENCE_UPGRADE_BRIEF.md). Keystone serves as the high-speed retrieval, indexing, topology expansion, and explainable decision-support accelerator for CITADEL and QIHSE without holding state authority.

### Phase 0: Federation Wire Envelope & Dual Ingestion — COMPLETED
- [x] **Canonical Federation Envelope (`include/keystone_federation.h`)**:
  - Defined `keystone_federation_record_t` mapping `source_object_id`, `source_event_id`, `source_node_id`, `source_generation`, `fencing_epoch`, `source_hlc`, `tenant_id`, `classification`, `object_type`, and record `flags` (`TOMBSTONE`, `SNAPSHOT`, `EVENT`, `TELEMETRY`, `AUDIT`, `SECURITY_SENSITIVE`).
- [x] **UUID & Hybrid Logical Clock (HLC) C Primitives**:
  - Implemented 128-bit `keystone_uuid_t` with string parser, formatter, comparison, and 64-bit avalanche hash.
  - Implemented monotonic `keystone_hlc_t` with wall-clock skew tolerance, causal update propagation, and tie-breaking.
- [x] **Hostile-Input-Safe Wire Serialization (`src/federation/keystone_federation_ingest.c`)**:
  - 124-byte packed binary wire format (`KEYSTONE_FEDERATION_ENVELOPE_MAGIC = 0x4B534645`), CRC32-checked header and payload, bounded allocation safeguards (<64 MiB payload cap), and tested corrupted-byte rejections.
- [x] **Idempotent Ingestion & Deduplication**:
  - High-throughput deduplication hash ring rejecting duplicate event UUIDs idempotently. Benchmarked at **9.3+ million events/sec** (107 ns/event).
- [x] **Tombstone Registry & Fencing Epoch Protection**:
  - Instant negative lookup masking via `keystone_federation_is_tombstoned()` upon receiving tombstone flags. Rejection of stale fencing epochs (`KEYSTONE_INGEST_STALE_FENCING_EPOCH`).
- [x] **Atomic Checkpoint Persistence**:
  - Double-buffered atomic persistence (`.tmp` + `fsync()` + `rename()`) with CRC32 verification restoring engine watermark, epoch, and generation states.
- [x] **Explainable Recommendation Bundles**:
  - Defined `keystone_recommendation_t` and `keystone_explain_t` structures with structured hard-constraint pass/fail audits and score provenance.

### Phase 1: Infrastructure Exact Identity & Monotonic Temporal Indexing — COMPLETED
- [x] **Exact Identity Directory (`include/keystone_exact_index.h`, `src/federation/keystone_exact_index.c`)**:
  - Fast collision-safe $O(1)$ open-addressing mapping from resource/event/node UUID to active index generation, fencing epoch, HLC, flags, and location slot.
  - 128-bit key verification eliminating hash ambiguity, dynamic power-of-two resizing, active vs tombstone count tracking, stale generation/epoch rejection, and CRC32-verified atomic persistence.
- [x] **Monotonic HLC Temporal Timeline Index (`include/keystone_temporal.h`, `src/temporal/keystone_temporal_index.c`)**:
  - Monotonic `(HLC, event_id, object_id, event_type)` index with branchless binary lower/upper bound searches.
  - Range search benchmarked at **3.7+ million queries/sec (269 ns/query)**.
  - Object-scoped timeline queries, reverse-chronological incident scans, time-bucket histogram aggregations, out-of-order arrival stabilization, and CRC32-verified atomic persistence.

### Phase 2: Security-Aware Native Service Mode (`keystoned`) — COMPLETED
- [x] **Unprivileged Service Daemon (`bin/keystoned`, `src/service/keystoned_main.c`, `src/service/keystoned_server.c`)**:
  - Standalone daemon and server communicating over local Unix domain sockets (`AF_UNIX`) with restricted `0700` socket permissions.
  - Multi-threaded poll worker and compact binary IPC framing with CRC32 header and payload integrity validation.
- [x] **Security Context Partitioning & Client IPC (`include/keystoned.h`, `src/service/keystoned_client.c`)**:
  - Clearance and compartment bitmask verification on every lookup and timeline range query (`keystone_security_check`).
  - Strict isolation: callers lacking clearance receive instant `KEYSTONED_STATUS_DENIED` and sensitive records are completely filtered from temporal queries.
- [x] **Atomic Generation Publication**:
  - Reader-writer generation state pointer swap (`keystoned_server_publish_generation`) enabling offline index construction and atomic, zero-downtime publication.

### Phase 3: Topology Graph Cache & Hybrid Query Planning — COMPLETED
- [x] **Adjacency Matrix Caching (`include/keystone_topology.h`, `src/topology/keystone_topology_index.c`)**:
  - Read-optimized graph index for infrastructure relations (`RUNS_ON`, `ATTACHED_TO`, `ROUTES_THROUGH`, `DEPENDS_ON`, `REPLICATED_TO`, `SHARES_FAILURE_DOMAIN`).
  - Node metrics tracking (CPU ISA features, RAM headroom, CPU load, thermal temperature, NUMA topology, failure domains, security classification).
  - Graph traversal primitives: neighborhood expansion, failure domain clustering, dependency chain tracing, and affected resource mapping.
- [x] **Two-Tier Query Planner & Recommendation Engine (`include/keystone_hybrid.h`, `src/query/keystone_hybrid_planner.c`)**:
  - Tier 1: Hard constraint evaluation with boolean pruning (security clearance, required CPU ISA flags, minimum RAM/cores, failure domain anti-affinity).
  - Tier 2: Soft objective ranking (weighted normalized free RAM, thermal headroom, CPU load, NUMA locality).
  - Emits explainable recommendation bundles (`keystone_recommendation_t`, `keystone_explain_t`) with full pass/fail constraint rationales and generation/HLC decision provenance.

### Phase 4 & 5: Streaming Telemetry & Hardware Silicon Acceleration — COMPLETED
- [x] **Streaming Telemetry Windows (`include/keystone_telemetry.h`, `src/telemetry/keystone_telemetry_engine.c`)**:
  - High-throughput circular ring buffer ingestion with per-node and per-metric online statistical state tracking.
  - Multi-tier rolling feature extraction windows (1m, 5m, 15m, 1h, 24h) computing mean, variance, stddev, EWMA, min/max bounds, linear regression slope/trend ($dv/dt$), burst counts, and error rates.
  - Multi-stage deterministic anomaly engine: static warning/critical threshold violation and statistical z-score outlier detection ($|z| \ge 3.0$).
  - Structured anomaly evidence bundles (`keystone_explain_t`) capturing baseline mean/stddev, observed deviations, severity ratings, and generation/HLC audit provenance.
- [x] **Hardware Silicon Acceleration & Incident Similarity Search (`include/keystone_incident.h`, `src/incident/keystone_incident_engine.c`)**:
  - Normalized vector embedding engine synthesizing failure patterns from streaming telemetry metrics, slope deltas, ISA features, and error tokens.
  - Multi-tier hardware silicon acceleration dispatch: NVIDIA CUDA GPU batch distance, Intel Sapphire Rapids AMX quantized tile matrix multiplication (`_tile_dpbssd`), AVX-512 vector FMA, AVX2+FMA SIMD, and guaranteed Scalar CPU reference fallback.
  - Historical incident matching emitting advisory mitigation recommendations (`keystone_recommendation_t`) with complete audit evidence and strict non-authoritative invariants.
- [x] **Comprehensive Hardware & Integration Test Suite (`tests/test_telemetry_incident_engine.c`)**:
  - Validates static threshold alarms, statistical z-score outliers, linear slope trends, historical incident matching, cross-backend bit accuracy (Scalar vs AVX2), and non-authoritative recommendation bundles.

### Phase 6: Federated Distributed Query & Partial Result Merging — COMPLETED
- [x] **Multi-Node Fan-Out & Routing Coordinator (`include/keystone_federated_query.h`, `src/federation/keystone_federated_query.c`)**:
  - Remote node registry tracking indexer nodes, sites, latency, index generations, and live health status (`HEALTHY`, `DEGRADED`, `UNREACHABLE`).
  - Strict conflict resolution across distributed nodes: resolves identity collisions via fencing epoch priority, source generation, and monotonic HLC tie-breakers (`keystone_federated_merge_exact_identity`).
- [x] **Partial-Result Merging & Partition Resilience**:
  - Multi-way monotonic timeline merge (`keystone_federated_merge_timelines`) preserving strict ascending HLC ordering and deduplicating events across nodes.
  - Resilient top-K similarity search aggregation (`keystone_federated_merge_topk_incidents`) with rank preservation, deduplication, and partial execution tracking (`keystone_partial_status_t`).
  - Graceful degradation: queries never fail completely when nodes time out or partition.

### Phase 7: AI/RAG Context Retrieval & Model Governance — COMPLETED
- [x] **Structured Evidence Context Packs (`include/keystone_rag.h`, `src/query/keystone_rag_engine.c`)**:
  - High-performance constrained AI retrieval endpoint (`keystone_rag_query_context`) extracting complete infrastructure context packs (`keystone_context_pack_t`).
  - Assembles timeline changes, rolling telemetry feature summaries, active anomalies, similar historical incidents, and topology neighborhood relations.
  - Strict security context partitioning: verifies caller clearance and compartment bitmasks, denying access with zero metadata leakage.
  - Explicit traceable citations (`keystone_citation_t`) attaching origin node, source object UUID, generation, and HLC to every piece of context evidence.
- [x] **Model Governance Engine**:
  - Formal model registry (`keystone_model_manifest_t`) validating model versions, supported task capabilities (`KEYSTONE_TASK_PLACEMENT`, `ANOMALY`, `SIMILARITY`, `RAG`), and cryptographic SHA-256 weight hash integrity.
  - Prevents unversioned, untracked, or tampered models from influencing infrastructure recommendations.
- [x] **Comprehensive Federated Query & RAG Test Suite (`tests/test_federated_query_rag.c`)**:
  - Validates coordinator registry, conflict resolution, multi-way timeline merging, top-K incident aggregation, security denial, context pack extraction, and model governance validation.
