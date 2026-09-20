# Changelog

All notable changes to the KEYSTONE search engine are documented in this file.

## [2.0.0] - 2026-09-20

### CITADEL Federation Intelligence Upgrade (Phases 0–7 Complete)
- **Federation Wire Envelope & Dual Ingestion** (`include/keystone_federation.h`, `src/federation/keystone_federation_ingest.c`):
  - Defined packed 124-byte wire envelope (`KEYSTONE_FEDERATION_ENVELOPE_MAGIC = 0x4B534645`), IEEE 802.3 CRC32 header and payload checksums.
  - Implemented 128-bit UUID primitives (`keystone_uuid_t`) with RFC 4122 formatting and 64-bit avalanche hashing.
  - Implemented monotonic Hybrid Logical Clock (`keystone_hlc_t`) with causal advancement and deterministic tie-breaking.
  - Implemented high-throughput deduplication hash ring benchmarked at **9.3+ million events/sec** (107 ns/event).
  - Implemented instant tombstone registry masking, fencing epoch protection, and atomic double-buffered persistence (`.tmp` + `fsync` + `rename`).
- **Exact Identity Directory & Monotonic Temporal Indexing** (`include/keystone_exact_index.h`, `include/keystone_temporal.h`):
  - Fast collision-safe open-addressing exact identity index benchmarked at **4.6+ million queries/sec** (217 ns/query).
  - 64-byte cache-line aligned monotonic temporal timeline index benchmarked at **3.7+ million range queries/sec** (269 ns/query).
  - Object-scoped timeline queries, time-bucket histogram aggregations, out-of-order arrival stabilization, and CRC32-verified persistence.
- **Security-Aware Native Service Mode (`keystoned`)** (`bin/keystoned`, `include/keystoned.h`, `src/service/*`):
  - Standalone unprivileged service daemon communicating over local Unix domain sockets (`AF_UNIX`) with restricted `0700` directory permissions.
  - Multi-threaded poll server and compact binary IPC framing with CRC32 integrity checks.
  - Multi-level security context partitioning (`clearance_level`, `compartment_mask`, `tenant_id`) with zero metadata leakage (`KEYSTONED_STATUS_DENIED`).
  - Reader-writer generation pointer swapping (`keystoned_server_publish_generation`) for zero-downtime atomic publication.
- **Topology Graph Cache & Two-Tier Hybrid Query Planner** (`include/keystone_topology.h`, `include/keystone_hybrid.h`):
  - In-memory graph adjacency cache (`RUNS_ON`, `ATTACHED_TO`, `ROUTES_THROUGH`, `DEPENDS_ON`, `REPLICATED_TO`, `SHARES_FAILURE_DOMAIN`).
  - Neighborhood expansion, failure domain clustering, and dependency chain blast radius tracing.
  - Two-tier hybrid query planner: Tier 1 boolean constraint pruning (security, CPU ISA flags, RAM, anti-affinity) and Tier 2 weighted soft ranking (RAM, CPU, thermals, NUMA).
  - Explainable recommendation bundles (`keystone_recommendation_t`, `keystone_explain_t`) with complete provenance and non-authoritative invariants.
- **Streaming Telemetry & Deterministic Anomaly Engine** (`include/keystone_telemetry.h`, `src/telemetry/*`):
  - High-throughput circular sample buffers with online Welford statistics (mean, variance, standard deviation).
  - Multi-tier rolling feature extraction windows (1m, 5m, 15m, 1h, 24h) computing EWMA, min/max bounds, linear regression slope/trend ($dv/dt$), burst counts, and error rates.
  - Deterministic multi-stage anomaly engine combining static threshold violation alarms with statistical $z$-score outlier detection ($|z| \ge 3.0$) and zero-variance boundary protection.
- **Silicon Acceleration & Historical Incident Similarity** (`include/keystone_incident.h`, `src/incident/*`):
  - 64-dimensional normalized incident vector embeddings synthesizing telemetry metrics, trends, ISA features, and error tokens.
  - Multi-tier hardware silicon acceleration dispatch: NVIDIA CUDA GPU batch distance, Intel Sapphire Rapids AMX (`_tile_dpbssd`), AVX-512 vector FMA, AVX2+FMA SIMD, and guaranteed Scalar CPU reference fallback.
  - Historical incident matching emitting advisory mitigation recommendations with strict non-authoritative invariants.
- **Federated Distributed Query Coordinator** (`include/keystone_federated_query.h`, `src/federation/keystone_federated_query.c`):
  - Multi-node coordinator tracking indexer nodes, sites, latency, index generations, and live health status (`HEALTHY`, `DEGRADED`, `UNREACHABLE`).
  - Strict conflict resolution across distributed nodes: Epoch $\succ$ Generation $\succ$ HLC.
  - Multi-way monotonic timeline merge and resilient top-K similarity search aggregation.
  - Partial-result degradation status (`keystone_partial_status_t`) ensuring partition tolerance.
- **AI/RAG Context Retrieval & Cryptographic Model Governance** (`include/keystone_rag.h`, `src/query/keystone_rag_engine.c`):
  - Constrained AI retrieval endpoint extracting structured context packs (`keystone_context_pack_t`) assembling timelines, telemetry, anomalies, incidents, and topology.
  - Explicit traceable citations (`keystone_citation_t`) attaching node of origin, source object UUID, generation, and HLC to every piece of context evidence.
  - Formal model governance registry (`keystone_model_manifest_t`) validating task capabilities and cryptographic SHA-256 weight hash integrity.

### Trigram Engine High-Throughput Upgrades & Standalone CLI (`bin/tgrep`)
- Pure C11 standalone grep replacement built on 24-bit direct directory indexing (`16M * 4 bytes` flat table for $O(1)$ zero-probe lookups).
- Multi-threaded parallel index construction (`keystone_trigram_index_build_parallel`) achieving **6.7x speedup** on 1GB corpora.
- Adaptive SIMD intersection combining blocked AVX2 with monotonic galloping search (`ks_lower_bound_gallop_u32`).
- Dense posting 64-bit word bitmaps with $O(1)$ bit test and bitwise AND.
- Direct binary index caching (`-I` / `.tgrep.idx`) achieving sub-millisecond query startup.

### Documentation & Verification
- Authored 14 comprehensive architecture documents in `docs/architecture/` covering all federation intelligence modules.
- Expanded test suite to **19 / 19 passing test suites** (`make check`) with zero compiler warnings under `-Wall -Wextra -Werror=implicit-function-declaration`.

## [1.4.0] - 2026-09-11

### Trigram Content Indexing Engine (tgrep-style)
- **Native Inverted Trigram Index** (`include/keystone_trigram.h`, `src/keystone_trigram.c`) — 24-bit hash trigram inverted posting list intersection engine providing sub-linear text/log candidate rejection.
- **SIMD Case-Insensitive Mode** — `KEYSTONE_TRIGRAM_OPT_CASE_INSENSITIVE` flag with SSE4.2 character folding and case-insensitive substring verification.
- **Binary Persistence** — Zero-rebuild `keystone_trigram_index_save()` and `keystone_trigram_index_load()` file format.
- **Streaming Archive Ingestion** — `keystone_tar_zst_index_trigram()` and `dsmil_trigram_index_tar_zst()` index compressed archive text members directly from stream buffers with zero disk inflation.
- **Python SDK Bindings** — Added `keystone.TrigramIndex` with support for document ingestion, case-insensitivity, streaming archive ingestion, binary persistence, and candidate document search.

### Calibration Cache & Workload Profiling
- **Workload Profile Detection** — Added `keystone_detect_auto_query_profile()` evaluating query shape (`general`, `dense_sorted`, `sparse_sorted`, `strided`, `random`), estimated in-bounds hit percentage, average key gap, and constant stride using 128-bit overflow-safe arithmetic.
- **Profile Fields in Decision Provenance** — Exposed `hit_rate_pct`, `avg_gap`, and `detected_stride` in public `keystone_backend_decision_t` and Python `BackendDecision`.
- **Granular Cache Keys** — Calibration cache (`g_backend_cache`) now keys on CPU features, array size bucket, query count bucket, thread count, query shape, 25%-granular hit-rate buckets, power-of-two gap buckets, and detected stride.
- **Fallback Policy Verification** — Added testing switches `KEYSTONE_FORCE_CALIBRATION_FALLBACK` and `KEYSTONE_DISABLE_CALIBRATION_CACHE` to verify graceful degradation to `KEYSTONE_DECISION_SOURCE_STATIC_FALLBACK` and cache bypass behavior without corrupting cached decisions.
- **Rich Host & Build Metadata** — Added `host` (nodename), `os` (sysname), `arch` (machine), `release` (kernel release), `compiler` (`gcc`/`clang`), and `compiler_version` (`__VERSION__`) across benchmark writers and CSV/JSON output.
- **Makefile Integration** — Added target `scripts/compare_search_auto` for direct automated benchmark execution.

## [1.3.0] - 2026-09-02

### Performance & Concurrency Hardening
- **Zero-Copy Pointer Ping-Pong Radix Sort** — Converted 8-pass LSD radix sort (`src/dsmil_hash_indexer.c`) to pointer ping-ponging across source and destination buffers. Completely eliminates 32 full-array `memcpy` operations, halving sort overhead on multimillion-record batches.
- **Vector Engine Zero-Heap Query Fast Paths** — Added stack buffer scratchpads for cosine query normalization (up to 1,024 dimensions) and LSH candidate reranking (up to 512 candidates) in `vector_engine/keystone_engine.c`. Completely removes heap allocations during batch search queries.
- **LSH SIMD Pipelining & Bloom Deduplication** — Unrolled `sign_dot` 4-way with parallel accumulator registers to saturate CPU FMA pipelines. Added 64-bit fast bloom filter in `vector_engine/lsh.c` candidate collection to reject duplicates in $O(1)$ time, skipping quadratic deduplication loops ~90% of the time. Upgraded bucket finalization from $O(N^2)$ bubble sort to $O(N \log N)$ `qsort`.
- **Compiler-Agnostic OpenMP Vector Engine Build** — Upgraded `vector_engine/build_vector_engine.sh` to probe for OpenMP using compiler compilation checks rather than relying on compiler binary names, enabling multi-core vector search by default.
- **Auto-Backend Reader-Writer Concurrency** — Upgraded `g_backend_cache_mutex` to `pthread_rwlock_t` (`g_backend_cache_rwlock`) in `src/keystone.c`. Multiple concurrent search workers query cached backend calibration decisions in parallel with zero lock contention.
- **QIHSE Bridge Invariant 1 Security Alignment** — Automatically routes credential dispatch to authenticated write path (`keystone_qihse_bridge_dispatch_credential_authenticated`) whenever an ingestion principal is configured, preventing context-free write bypasses.
- **Streaming Archive Multi-Archive Batch & Indexing** — Implemented rewind, sidecar indexing (`<archive>.idx.json`), pipelined decompression, and multi-archive pool APIs (`keystone_tar_zst_batch_*`) in `src/keystone_tar_zst.c`.

## [1.2.0] - 2026-08-28

### Security Fixes (P1/P2)
- **P1: tar.zst parser OOB read** — Replaced unbounded `strtoll()` with a bounded streaming integer parser that respects buffer length. Eliminates out-of-bounds read vulnerability in `.tar.zst` integer parsing.
- **P1: CUDA cache race** — Replaced spinlock-released-before-use pattern with a reader-lease protocol (refcount + generation). Eviction now waits for `readers == 0` before `cudaFree`. Added `keystone_search_batch_cuda_versioned()` with `dataset_version` for in-place host array mutation detection. Added `keystone_cuda_cache_invalidate()`.
- **P2: Archive index min/max** — `first_key`/`last_key` now recomputed from the sorted array after parsing, not trusted from stream order (wrong for unsorted source data).
- **P2: Auto-backend data races** — `g_backend_cache` and `g_last_backend_decision` protected by mutexes; `valid=1` published last after all fields written. Eliminates torn reads on concurrent access.
- **P2: Signed overflow in query-shape classifier** — All key deltas and `max-min` range computed in `__int128`, eliminating UB near `INT64_MIN`/`INT64_MAX`.
- **P2: FNV hash collision verification** — Hash indexer now retains original string bytes and verifies them on every positive hit, eliminating false matches from 64-bit hash collisions.
- **QIHSE ingestion principal** — Bridge carries an authenticated `ingestion_principal` via `keystone_qihse_bridge_set_principal()`. New `keystone_qihse_bridge_dispatch_credential_authenticated()` uses `qihse_kv_set_user()` and refuses writes without a principal, per QIHSE AGENTS.md invariant #1.

### Performance
- **Archive index keys retention** — Sorted keys retained in `tar_zst_index_entry`, eliminating repeat decompression/parsing for positive lookups. Fallback re-streams if keys not retained.
- **LSD radix sort** — Replaced `qsort` with 8-pass LSD radix sort (O(n), sequential memory access) for 64-bit hash keys, carrying offsets/strings/lens.
- **Zero-copy NumPy batch API** — New `keystone_search_keys_batch_auto()` takes raw `int64_t*` keys and `size_t*` results directly from NumPy buffers. Python `search_batch_keys()` skips per-key `_CBatchItem` marshalling. **15.6x faster** on 1M queries (0.151s vs 2.351s).

### Correctness
- **Anchor LRU tracking** — Endpoint anchors now initialize `use_count`/`last_used`; usage-update block no longer guards on `active_table != table`, so caller table anchors get LRU timestamps refreshed.

### Sandy Bridge / AVX1-only CPU Support
- **SSE4.2 SIMD path** — Added branchless 128-bit SIMD path (`_mm_cmpeq_epi64` / PCMPEQQ) to `keystone_chunked_search`, 2x unrolled for Sandy Bridge's dual 128-bit execution ports. Previously AVX1-only CPUs fell through to a scalar loop that couldn't auto-vectorize.
- **Double-precision interpolation** — Replaced `__int128` division (80-100+ cycle libgcc `__divti3` call) with double-precision fast path (~20-40 cycles). `__int128` fallback only for overflow edge cases. **2x faster single-key search** on Sandy Bridge.
- **Software prefetch enabled for SSE4.2** — Prefetch was `#ifdef`'d out on AVX1-only CPUs. Added SSE4.2 branch with Sandy Bridge-tuned distances (32/64 elements vs 64/128).
- **Branchless scalar fallback** — Removed early returns that blocked GCC auto-vectorization.
- **Wider SIMD scan window** — `keystone_local_search` uses 64-element window on SSE4.2+ (was fixed at 32).
- **OpenMP auto-enabled** — Makefile auto-detects compiler OpenMP support and enables `-fopenmp` by default. **2x faster batch search** on 8-core machines.
- **Lowered parallel threshold** — Auto-backend uses OpenMP for batches >= 4096 items (was 16384). Configurable via `KEYSTONE_AUTO_PARALLEL_MIN_ITEMS`.

### Benchmark Results (Sandy Bridge Xeon E5-2407, 2.2GHz, 8-core)
| Metric | Before | After | Speedup |
|--------|--------|-------|---------|
| Single-key search | 317 ns | 157 ns | 2.0x |
| Batch (serial) | 400 ns | 330 ns | 1.2x |
| Batch (auto+OpenMP) | N/A | 165 ns | 2.4x |
| Small-window scan | 125 ns | 82 ns | 1.5x |

## [1.1.0] - Upcoming

### API Changes
- **Auto-Backend Router**: Added `keystone_search_batch_auto` for intelligent, shape-aware batch query routing across Scalar, SIMD, and parallel boundaries.
- **Decision Provenance**: Exposed `keystone_backend_decision_t` and `keystone_get_last_backend_decision()` enabling full inspection of the router's hardware choices, calibration runs, and detected array shapes.
- **Archive Streamer API**: Introduced `keystone_tar_zst_t` and related methods for direct search over compressed archives (`.tar.zst`).
- **Telemetry Processing**: Introduced `dsmil_telemetry_processor.h` combining archive parsing with KEYSTONE's search.
- **Stable Metadata Documentation**: Promoted `keystone_backend_decision_t` fields to stable Doxygen comments.

### Benchmark Changes
- **CSV & JSON Reporting**: Integrated `-csv` and `-json` outputs in `dsmil_benchmark` and `performance_proof` for easier dashboard consumption.
- **Matrix Runner**: Created `run_perf_matrix.sh` with `perf stat` hardware counter tracking (cache misses, branch misses).
- **Archive Benchmarking**: Large archive extraction (`test_data.tar.zst`) now leverages `dsmil_search_batch_tar_zst` in batch workloads for extremely high-throughput telemetry simulation.
- **Host Capability Logs**: The benchmark suite now prints explicit capabilities (AVX2, AVX-512, AMX, Graviton4) prior to executing search workloads.

### Platform-Specific Behavior Changes
- **AVX-512 Isolation**: Separated the experimental `__AVX512F__` search logic out of the main compiler path into `keystone_avx512.c`, compiling securely with `-mavx512f -mavx512dq`.
- **AMX Constraints**: Bounded AMX usage strictly to "feature detection only," awaiting tile-stride integer search correctness validation.
- **Graviton4 Profiling**: Explicitly introduced Neoverse V2 caching topology logic locking anchors directly to the 2MB private L2 cache.
- **Build Mode Explicit Requirements**: Created robust dependency checks allowing fallback scalar implementations (`KEYSTONE_FORCE_SCALAR`) or fully parallel setups (`KEYSTONE_ENABLE_OPENMP`, `KEYSTONE_ENABLE_FORTRAN`).
