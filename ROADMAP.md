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
- [ ] **Arena-Backed Posting List Construction**:
  - Replace per-posting `malloc()` calls with chunked arena allocators during ingestion to eliminate heap fragmentation.
- [x] **Contiguous Flattened Index Layout**:
  - At `finalize()`, flatten all posting lists into a single contiguous memory pool (`flat_postings`), eliminating heap fragmentation and reducing index destruction to $O(1)$.
- [x] **Dense Posting Bitmap Conversion**:
  - Converted high-frequency trigrams ($\ge \text{doc\_count} / 32$, with $\ge 64$ docs) to 64-bit word bitmaps at `finalize()`. Intersect dense lists via $O(1)$ bit test and 64-bit word bitwise AND with `__builtin_ctzll`.
- [ ] **Direct 24-Bit Descriptor Directory**:
  - For large indices ($>2^{20}$ trigrams), replace the open-addressing hash table with a flat 64 MiB direct 24-bit descriptor directory (`16M * 4 bytes`), achieving $O(1)$ zero-probe trigram lookups.
- [x] **True Streaming Ingestion with 2-Byte Carry**:
  - Overhauled `keystone_trigram_stream` to carry the trailing 2 bytes across arbitrary buffer chunks with $O(1)$ buffer memory for external documents.

### Phase 3: Multi-Threaded Parallel Construction
- [ ] **Thread-Local Builders**:
  - Implement independent builder contexts per worker thread with contiguous document ID ranges (`[doc_start, doc_end)`).
- [ ] **Parallel Merge Pass**:
  - Two-pass finalize: compute global trigram frequencies in parallel, allocate contiguous global posting pool, and parallel-copy thread-local postings into final sorted slices.

---

## 3. Backlog & Hardware Scaling

### Fortran & Scientific Search
- [ ] **Expanded Workload Benchmarking**: Evaluate Fortran `keystone_batch_search_i64` on sparse and strided access patterns beyond dense all-hit sets.
- [ ] **Auto-Route Widening**: Conditionally expand auto-calibration selection when Fortran demonstrates $>10\%$ repeatable wins.

### Memory Ramp & Cold-Cache Verification
- [ ] **RAM-Capacity Sweeps**: Formalize benchmark runs at 5%, 10%, 25%, 40%, and 60% of `MemAvailable` with resident set size (RSS) and page fault tracking.
- [ ] **OS Cold-Cache Profiling**: Implement opt-in cache-drop test suites (`/proc/sys/vm/drop_caches`).

### Future Silicon
- [ ] **AMX Matrix Acceleration**: Research tiled integer matrix search primitives on supported hardware.
- [ ] **Resident GPU/NPU Stress Testing**: Continuous hardware-in-the-loop validation of CUDA and Myriad X VPU vector kernels.

---

## 4. Downstream Integrations

- [ ] **Parrot-Sabot (`SWORDIntel/parrot-sabot`)**:
  - Integrate `TrigramIndex` into `ArtifactSpooler` (`sabot/artifacts.py`) for `/search-artifacts <query>` sub-millisecond log triage.
- [ ] **QIHSE AI Compute Fabric (`SWORDIntel/QIHSE`)**:
  - Export Keystone engine capability frames (`NODE_CAP` frame 8u) to the QIHSE cluster bus.
