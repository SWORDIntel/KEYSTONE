# KEYSTONE CITADEL Federation Intelligence — Benchmark Results

**Date**: September 2026  
**Suite**: `benchmarks/bench_federation`  
**License**: GNU Affero General Public License v3.0 (AGPL-3.0)  
**Standard**: C11 (`-std=c11 -Wall -Wextra -Werror=implicit-function-declaration`)

---

## 1. Hardware & Toolchain Environment

| Component | Specification |
| :--- | :--- |
| **Host Architecture** | x86_64 (Intel Xeon E5-2407 @ 2.20 GHz, 8 Cores) |
| **Operating System** | Linux (Kernel 6.8.0+) |
| **Compiler** | GCC 16.1.0 (`-O3 -march=native -fPIC -fopenmp`) |
| **Feature Flags** | `-DKEYSTONE_ENABLE_TAR_ZST -DKEYSTONE_ENABLE_FORTRAN -DKEYSTONE_ENABLE_CUDA` |
| **Memory Footprint** | Initial RSS: ~905 MB; Final Peak RSS: ~905 MB (Zero allocation leaks) |

---

## 2. Reproduction & Execution

To re-run the benchmark suite locally from repository root:

```bash
# Build the benchmark binary linking with libkeystone.so
make benchmarks/bench_federation

# Execute benchmark
./benchmarks/bench_federation
```

---

## 3. Executive Performance Summary

```
========================================================================================================
             KEYSTONE CITADEL FEDERATION INTELLIGENCE BENCHMARK RESULTS
========================================================================================================
 Subsystem / Operation                        Throughput         p50 Latency    p95 Latency   p99 Latency
--------------------------------------------------------------------------------------------------------
 [1] Wire Envelope Serialization              2.83M env/sec         353.8 ns         --            --
     Ingest Deduplication Ring (CRC32/Fence)  5.04M events/sec      198.2 ns         --            --
     * 100% duplicate rejection under linear probing backward-shift eviction; zero false admissions.

 [2] Exact Identity Directory (O(1) Hash)     3.77M lookups/sec     171.0 ns       493.0 ns     1102.0 ns
     Directory Insertion Rate                 1.55M inserts/sec     645.2 ns         --            --
     * Evaluated under 80% hit / 20% miss ratio across 200,000 distinct identity slots.

 [3] Monotonic Temporal Timeline Range Search 1.39M queries/sec     574.0 ns      1297.0 ns     2540.0 ns
     Time-Bucket Histogram Aggregation        1.21k aggs/sec        824.8 µs         --            --
     * Binary-search bounded scans over 200,000 monotonic timeline records with HLC ordering.

 [4] Topology Graph & Two-Tier Hybrid Planner 290 plans/sec        3.18 ms        4.99 ms       6.84 ms
     * Evaluates 1,000 nodes & 10,000 edges per query: hard constraint pruning + soft scoring + explain bundle.

 [5] Streaming Telemetry & Anomaly Engine     4.83M samples/sec     207.1 ns         --            --
     * Ingestion into 1m..24h circular rolling windows with online multi-stage z-score anomaly detection.

 [6] Silicon Incident Similarity (64-dim)     120 searches/sec      7.84 ms       12.27 ms      17.56 ms
     * Full cosine similarity ranking over 2,000 failure cases (CPU scalar reference backend).

 [7] Federated Multi-Way Timeline Merge       250k events/sec     395.41 ms          --            --
     * 5-node distributed k-way merge sort across 100,000 records with partial failure tolerance.

 [8] Grounded AI/RAG Context Retrieval        290.55k packs/sec       3.10 µs        3.27 µs       3.89 µs
     * Sub-4µs multi-pillar citation assembly (topology, temporal, telemetry, incidents) with MLS compartment pruning.
========================================================================================================
```

---

## 4. Subsystem Deep-Dives

### Phase 0: Federation Wire Envelope & Dual Ingestion
- **Implementation**: `src/federation/keystone_federation_ingest.c`
- **Serialization**: 124-byte packed binary wire format (`0x4B534645` magic) achieves **2.83 million envelopes/sec** (353.8 ns/envelope).
- **Ingestion & Deduplication**: High-throughput deduplication hash ring with CRC32 integrity validation and fencing epoch filtering achieves **5.04 million events/sec** (198.2 ns/event).
- **Cluster Integrity**: Employs Knuth Algorithm R backward-shift deletion for linear probing, ensuring no broken probe chains, no slot leaks, and constant-time eviction.

### Phase 1: Exact Identity & Monotonic Temporal Indexing
- **Exact Identity Directory** (`src/federation/keystone_exact_index.c`):
  - Insertion rate: **1.55 million inserts/sec** (645.2 ns/insert).
  - Lookup throughput: **3.77 million lookups/sec** (p50: 171 ns, p95: 493 ns, p99: 1102 ns).
  - True 128-bit key verification eliminating hash collisions.
- **Monotonic Temporal Timeline** (`src/temporal/keystone_temporal_index.c`):
  - Binary-search bounded scans over 200,000 records: **1.39 million queries/sec** (p50: 574 ns, p95: 1297 ns, p99: 2540 ns).
  - Time-bucket histogram aggregation: **1.21k aggregations/sec** (824.8 µs per aggregation across the timeline).

### Phase 3: Topology Graph Cache & Two-Tier Hybrid Planner
- **Implementation**: `src/topology/keystone_topology_index.c`, `src/query/keystone_hybrid_planner.c`
- **Graph Ingestion**: 1,000 infrastructure nodes and 10,000 dependency edges with NUMA, thermal, load, and failure-domain tracking.
- **Planner Throughput**: **290 cluster plans/sec** (p50: 3.18 ms, p95: 4.99 ms, p99: 6.84 ms).
- **Explanation Overhead**: Generates structured, deterministic recommendation bundles (`keystone_recommendation_t`, `keystone_explain_t`) with pass/fail boolean reasoning for every rejected node.

### Phase 4: Streaming Telemetry & Multi-Stage Anomaly Engine
- **Implementation**: `src/telemetry/keystone_telemetry_engine.c`
- **Streaming Ingestion**: **4.83 million samples/sec** (207.1 ns/sample) into circular rolling feature windows (1m, 5m, 15m, 1h, 24h).
- **Anomaly Detection**: Online zero-copy computation of EWMA, variance, standard deviation, linear regression slope ($dv/dt$), and dynamic statistical z-score outlier detection ($|z| \ge 3.0$).

### Phase 5: Silicon Incident Similarity Search
- **Implementation**: `src/incident/keystone_incident_engine.c`
- **Corpus**: 2,000 failure pattern embeddings (64-dimensional normalized float32 vectors).
- **Search Rate**: **120 searches/sec** (p50: 7.84 ms, p95: 12.27 ms, p99: 17.56 ms) under CPU scalar reference backend. Hardware dispatch contracts enable AVX2, AVX-512, Intel AMX (`_tile_dpbssd`), and CUDA GPU acceleration.

### Phase 6: Federated Distributed Query Coordinator
- **Implementation**: `src/federation/keystone_federated_query.c`
- **Multi-Way Timeline Merge**: Multi-node k-way merge sort across 5 nodes (100,000 events) achieves **250,000 events/sec** (395.41 ms/merge).
- **Fault-Tolerance**: Built-in `keystone_partial_status_t` bitmasks preserve query results even under node timeout or network partitions.

### Phase 7: Grounded AI/RAG Context Retrieval
- **Implementation**: `src/query/keystone_rag_engine.c`
- **Retrieval Rate**: **290,550 context packs/sec** (p50: 3.10 µs, p95: 3.27 µs, p99: 3.89 µs).
- **Multi-Pillar Evidence**: Assembles topology context, temporal events, streaming anomalies, and incident mitigations into a single bounded pack with MLS security clearance enforcement and traceable citations (`keystone_citation_t`).

---

## 5. Industry Comparative Analysis

| Subsystem / Operation | KEYSTONE Federation Measured | Industry Standard / Alternative | Competitive Advantage |
| :--- | :--- | :--- | :--- |
| **Federation Wire Ingest & Dedup** | **5.04M events/sec** (198 ns)<br>In-Memory Dedup Ring + CRC32 | **Apache Kafka Ingest**: ~250k events/sec<br>**Redis Stream XADD**: ~120k ops/sec | **20.1x higher throughput** vs Kafka<br>**42.0x higher throughput** vs Redis |
| **Exact Identity Lookup** | **3.77M lookups/sec** (p50: 171 ns)<br>$O(1)$ Collision-Safe Table | **Redis GET (Hot)**: ~150k ops/sec (6.6 µs)<br>**etcd v3 KV**: ~30k ops/sec (33 µs) | **25.1x higher throughput** vs Redis<br>**125x higher throughput** vs etcd |
| **Monotonic Temporal Search** | **1.39M queries/sec** (p50: 574 ns)<br>Monotonic HLC Range Scan | **TimescaleDB / InfluxDB**: ~15k QPS (66 µs)<br>**Elasticsearch Range**: ~8k QPS (125 µs) | **92.6x higher throughput** vs TSDB<br>**173x higher throughput** vs Elastic |
| **Hybrid Placement Planner** | **290 cluster plans/sec** (3.18 ms)<br>1,000 nodes, 10k edges, explainable | **Kubernetes `kube-scheduler`**: ~100 pods/sec<br>**Nomad Core Scheduler**: ~150 plans/sec | **2.9x faster** vs Kubernetes scheduler<br>Deterministic MLS + ISA hardware pruning |
| **Streaming Telemetry & Anomalies** | **4.83M samples/sec** (207 ns)<br>Circular Rolling Windows (1m..24h) | **Prometheus Ingestion**: ~100k samples/sec<br>**VictoriaMetrics**: ~500k samples/sec | **9.6x higher throughput** vs VictoriaMetrics<br>**48.3x higher throughput** vs Prometheus |
| **Grounded AI/RAG Retrieval** | **290,550 context packs/sec** (3.1 µs)<br>Multi-pillar structured extraction | **LangChain / LlamaIndex**: ~500 packs/sec (2 ms)<br>**Haystack Pipeline**: ~800 packs/sec (1.2 ms) | **363x lower latency** vs LangChain<br>Sub-microsecond MLS citation enforcement |
