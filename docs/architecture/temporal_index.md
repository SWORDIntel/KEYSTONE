<!--
  SPDX-License-Identifier: AGPL-3.0-or-later
  Copyright (C) 2026 SWORDIntel. All rights reserved.
-->

# Monotonic Temporal Indexing Architecture

This document specifies the internal memory layout, binary search algorithms, out-of-order event stabilization, object-scoped timeline filtering, and time-bucket aggregations in the KEYSTONE Monotonic Temporal Index subsystem.

Governed by Phase 1 of [`CITADEL/docs/architecture/KEYSTONE_FEDERATION_INTELLIGENCE_UPGRADE_BRIEF.md`](../../../CITADEL/docs/architecture/KEYSTONE_FEDERATION_INTELLIGENCE_UPGRADE_BRIEF.md), the temporal index provides high-throughput causal ordering and sub-microsecond range queries across billions of infrastructure events.

---

## 1. Design Overview & Requirements

CITADEL infrastructure generates millions of telemetry, lifecycle, and security events per second across distributed nodes. Traditional B-trees or LSM trees exhibit write amplification and lock contention under high-ingest monotonic streams.

KEYSTONE's temporal index is designed for:
- **Monotonic Causal Ordering**: Indexed strictly by Hybrid Logical Clock (`keystone_hlc_t`), ensuring causal consistency without clock-skew vulnerabilities.
- **Cache-Aligned Contiguous Memory**: Packed array structure maximizing L1/L2 cache line utilization.
- **Sub-Microsecond Range Queries**: Monotonic lower/upper bound binary search (measured throughput is host-dependent — see [`benchmarks/FEDERATION_BENCHMARK.md`](../../benchmarks/FEDERATION_BENCHMARK.md)).
- **Out-of-Order Arrival Stabilization**: Handles bounded network jitter and clock skew without corrupting timeline monotonicity.
- **Zero-Allocation Query Filtering**: In-place filtering by target object UUID, event type bitmasks, and tenant security domains.

---

## 2. Memory Layout & Structures

Defined in [`include/keystone_temporal.h`](../../include/keystone_temporal.h):

```c
typedef struct {
    keystone_hlc_t hlc;              /* 16 bytes: physical_ms, logical, node_id */
    keystone_uuid_t event_id;        /* 16 bytes: unique event identifier */
    keystone_uuid_t object_id;       /* 16 bytes: entity identifier (VM, Node) */
    uint32_t event_type;             /* 4 bytes: object type classification */
    uint32_t tenant_id;              /* 4 bytes: tenant isolation boundary */
    uint32_t classification;         /* 4 bytes: security classification */
    uint32_t flags;                  /* 4 bytes: record flags (tombstone, audit, ...) */
    uint64_t source_generation;      /* 8 bytes: generation of origin */
    uint64_t payload_offset;         /* 8 bytes: spool/journal payload location */
} keystone_temporal_entry_t;         /* Total: 80 bytes (aligned to 8) */
```

### Contiguous Packed Array
Each `keystone_temporal_entry_t` occupies exactly 80 bytes. Entries are stored in one
contiguous sorted array:
- Sequential iteration and prefetching stream whole temporal records per cache-line run.
- SIMD and vector registers can inspect multiple entry headers in parallel.

```c
/* src/temporal/keystone_temporal_index.c (internal) */
typedef struct {
    keystone_temporal_entry_t* entries; /* Contiguous, sorted; capacity slots allocated */
    size_t capacity;                    /* Allocated capacity (always ≥ allocated slots) */
    size_t count;                       /* Current entry count (≤ capacity) */
} keystone_temporal_index_t;
```

**Invariant (enforced since the 2026-09-28 overflow fix):** the entries array is allocated
for *capacity* slots — `count <= capacity <= allocated`. A loaded index must never claim more
capacity than it allocated, or `append` writes past the array (the production R6b feed bug).

---

## 3. Monotonic Timeline Range Search

Given a time range $[HLC_{start}, HLC_{end}]$, `keystone_temporal_query_range()` executes branchless binary searches to locate the bounding indices:

```mermaid
flowchart LR
    subgraph Sorted Timeline Array
        E0["Entry 0\nHLC: 100"]
        E1["Entry 1\nHLC: 105"]
        E2["Entry 2 (lower_bound)\nHLC: 110"]
        E3["Entry 3\nHLC: 115"]
        E4["Entry 4\nHLC: 120"]
        E5["Entry 5 (upper_bound)\nHLC: 125"]
        E6["Entry 6\nHLC: 130"]
    end
    Q["Query Range:\n[110, 125]"] --> E2
    Q --> E5
    E2 -.->|Slice [2..5]| Res["Fast Sequential Scan / Filter"]
```

### Algorithm Complexity
1. **Lower Bound**: $O(\log N)$ binary search finding the first entry where $\text{HLC} \ge HLC_{start}$.
2. **Upper Bound**: $O(\log N)$ binary search finding the first entry where $\text{HLC} > HLC_{end}$.
3. **Candidate Scan**: The resulting contiguous slice $[idx_{start}, idx_{end})$ is scanned directly from contiguous RAM with zero intermediate heap allocations.

Under benchmark workloads on the 2026-09-20 benchmark host, range queries achieved **1.39 million lookups per second** (median latency: 574 ns); a 2026-09-28 audit re-run on a loaded container measured 0.94M/s (p50 920 ns). See [`benchmarks/FEDERATION_BENCHMARK.md`](../../benchmarks/FEDERATION_BENCHMARK.md).

---

## 4. Object-Scoped Timeline Retrieval

In incident investigations and VM lifecycle audits, operators query all events associated with a specific resource across time:

```c
uint32_t keystone_temporal_query_object(
    const keystone_temporal_index_t *index,
    const keystone_uuid_t *object_id,
    keystone_hlc_t start_hlc,
    keystone_hlc_t end_hlc,
    keystone_temporal_entry_t *out_entries,
    uint32_t max_results);
```

### Tombstone-Aware Active History (§28)

`keystone_temporal_index_query_object_active()` is the resurrection-safe
variant used by serving paths: it returns entries only when the object's
most recent event at or before the query's `hlc_max` does **not** carry
`KEYSTONE_RECORD_FLAG_TOMBSTONE`. Deleted objects return zero entries;
re-creation (a newer non-tombstone event) clears the deleted state; the raw
`query_object` keeps serving the full audit history including deletion
events. `keystone_temporal_index_object_is_deleted()` exposes the as-of
deletion test directly. Deletion state derives from the timeline itself, so
it survives persistence with the entries.

### Freshness (§36)

`keystone_temporal_index_watermark()` returns the index high-water mark
(newest HLC + entry count); `keystone_hlc_staleness_ms()` computes wall-clock
staleness for any source HLC. Together these are the uniform freshness
contract shared with the RAG packs and keystoned responses.

### Query Execution Steps:
1. Performs range bounding $[idx_{start}, idx_{end})$ on the monotonic timeline.
2. Evaluates the candidate slice using vectorized 128-bit UUID comparisons (`_mm_cmpeq_epi64` / branchless SIMD).
3. Copies matching entries directly into `out_entries` up to `max_results`.
4. Returns the exact number of matching records found.

---

## 5. Time-Bucket Histogram Aggregations

To support real-time dashboards and anomaly burst detection, the temporal engine aggregates event frequencies into discrete time buckets without copying event bodies:

```c
typedef struct {
    uint64_t bucket_start_ms;
    uint64_t bucket_end_ms;
    uint64_t event_count;
} keystone_time_bucket_t;

int keystone_temporal_aggregate_histogram(
    const keystone_temporal_index_t *index,
    uint64_t start_ms,
    uint64_t end_ms,
    uint64_t bucket_duration_ms,
    keystone_time_bucket_t *out_buckets,
    uint32_t max_buckets,
    uint32_t *out_bucket_count);
```

### Optimization Characteristics
- Divides $[start\_ms, end\_ms]$ into uniform buckets (e.g., 1000 ms, 60000 ms).
- Direct index calculation: for each entry in range, computes $\text{bucket\_idx} = (\text{entry.physical\_ms} - start\_ms) / bucket\_duration\_ms$.
- Increments bucket counts in an $O(K)$ pass over the slice, where $K$ is the number of events in the time range.

---

## 6. Out-of-Order Arrival Stabilization

In federated systems, network delay can cause events with earlier HLC timestamps to arrive slightly after events with later timestamps:

```text
Stream Arrival Order:
  [HLC: 100] -> [HLC: 105] -> [HLC: 102 (Delayed)] -> [HLC: 110]
                                      ▲
                                 Out of Order
```

### Stabilization Mechanics
1. **Append Phase**: Inbound events are appended to the index buffer. If `entry->hlc < index->max_hlc`, the index marks `is_sorted = false`.
2. **Quicksort / Radix Merge**: Prior to publishing or serving range queries:
   - If the unsorted delta is small ($< 1024$ events), the engine executes an in-place insertion walk.
   - For larger deltas, the engine performs an in-place dual-pivot quicksort with fallback to LSD radix sort.
3. **Causal Convergence**: Once sorted, `is_sorted` is set to `true`, restoring $O(\log N)$ binary search invariants.

---

## 7. Atomic Persistence & Checkpointing

The temporal index is persisted to disk using double-buffered atomic checkpoints (`keystone_temporal_index_save()` / `keystone_temporal_index_load()`):

1. Header is written containing `magic = 0x4B53544D` (`'KSTM'`), index count, min/max HLC, and IEEE 802.3 CRC32 checksum.
2. Contiguous entry arrays are streamed directly to disk (`.tmp`).
3. `fsync()` guarantees durability.
4. Atomic `rename()` replaces the active checkpoint.
5. On load (hostile-input safe, hardened 2026-09-28): magic/version/strict-v1 header validation,
   checked arithmetic on the declared count, an **exact file-size bound (`fstat`) before any
   allocation** — truncated, trailing-garbage, and lying-count files are rejected — and the
   entries array is allocated for `max(count, 16)` capacity slots so append after resume always
   has the capacity the index claims. CRC32 over header + declared payload is verified before
   the index is returned.
