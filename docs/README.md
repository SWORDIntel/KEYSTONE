<!--
  SPDX-License-Identifier: AGPL-3.0-or-later
  Copyright (C) 2026 SWORDIntel. All rights reserved.
-->

# KEYSTONE Documentation

The root [README](../README.md) is intentionally written as a high-level introduction. Use this directory for implementation, integration, benchmark, architecture, and engineering detail.

## Start here

| Document | Use it for |
|---|---|
| [TECHNICAL_OVERVIEW.md](TECHNICAL_OVERVIEW.md) | Comprehensive architecture, search model, backend selection, federation intelligence pipeline, memory behavior, and QIHSE bridge. |
| [STATUS_SUMMARY.md](STATUS_SUMMARY.md) | Implementation status across all core search, trigram, vector, and CITADEL Federation Intelligence phases. |
| [BUILD_MODES.md](BUILD_MODES.md) | Native, scalar, OpenMP, optional Fortran, archive, daemon (`keystoned`), and standalone CLI (`tgrep`) builds. |
| [INTEGRATION.md](INTEGRATION.md) | Embedding KEYSTONE into another application, CITADEL control plane, or data pipeline. |
| [BENCHMARK_RESULTS.md](BENCHMARK_RESULTS.md) | Current measurement rules, calibration benchmarks, and throughput proofs. |
| [ACCELERATOR_CONTRACT.md](ACCELERATOR_CONTRACT.md) | Requirements a GPU/NPU/AMX accelerator backend must satisfy before it is treated as supported. |
| [TELEMETRY_PROCESSOR.md](TELEMETRY_PROCESSOR.md) | Streaming telemetry processor implementation and usage. |
| [TRIGRAM_BENCHMARK.md](TRIGRAM_BENCHMARK.md) | Inverted trigram index performance and benchmark analysis. |

---

## CITADEL Federation Intelligence Architecture (`docs/architecture/`)

Detailed design and specification documents for KEYSTONE's role as the federation-aware indexing and intelligence accelerator for CITADEL OS and QIHSE:

| Architecture Document | Focus Area |
|---|---|
| [federation_ingest.md](architecture/federation_ingest.md) | 124-byte wire envelope, magic/CRC32 validation, 9.3M ops/sec dedup ring, tombstone mechanics, and atomic checkpoint persistence. |
| [index_generations.md](architecture/index_generations.md) | Immutable generation lifecycle, atomic reader-writer pointer swapping (`publish_generation`), epoch watermarks, and offline rebuild workflows. |
| [security_partitioning.md](architecture/security_partitioning.md) | Security context model (`clearance_level`, `compartment_mask`), zero-metadata-leakage invariants, caller denial semantics, and unprivileged daemon isolation. |
| [temporal_index.md](architecture/temporal_index.md) | Monotonic HLC timeline index, 64-byte cache line alignment, 3.7M ops/sec binary range queries, object-scoped filtering, and time-bucket histograms. |
| [exact_and_temporal_indexing.md](architecture/exact_and_temporal_indexing.md) | Co-execution synergy between $O(1)$ exact identity directory and monotonic temporal history with conflict resolution. |
| [topology_index.md](architecture/topology_index.md) | In-memory graph adjacency cache (`RUNS_ON`, `DEPENDS_ON`, etc.), node hardware metrics (CPU ISA, RAM, thermals), neighborhood expansion, and blast radius calculation. |
| [hybrid_query.md](architecture/hybrid_query.md) | Two-tier query planner: Tier 1 boolean constraint pruning (security, ISA flags, RAM, anti-affinity) and Tier 2 weighted soft objective ranking. |
| [recommendation_engine.md](architecture/recommendation_engine.md) | Explainable recommendation bundles (`keystone_recommendation_t`, `keystone_explain_t`), scoring breakdown, decision provenance, and strict non-authoritative invariants. |
| [telemetry_and_anomaly_engine.md](architecture/telemetry_and_anomaly_engine.md) | Circular sample buffers, 1m/5m/15m/1h/24h rolling windows (mean, variance, EWMA, slope/trend), static thresholds, and $|z| \ge 3.0$ statistical outlier detection. |
| [silicon_and_incident_similarity.md](architecture/silicon_and_incident_similarity.md) | 64-dim incident vector embeddings, multi-tier hardware dispatch (CUDA, Intel AMX `_tile_dpbssd`, AVX-512, AVX2, Scalar reference), and top-K similarity search. |
| [federated_query_routing.md](architecture/federated_query_routing.md) | Multi-node coordinator, remote health tracking, distributed conflict resolution (epoch > generation > HLC), multi-way timeline merge, and partial degradation. |
| [rag_context_and_model_governance.md](architecture/rag_context_and_model_governance.md) | Constrained RAG context packs (`keystone_context_pack_t`), explicit citations (`keystone_citation_t`), and cryptographic model governance with SHA-256 weight hash validation. |
| [qihse_stream_contract.md](architecture/qihse_stream_contract.md) | Strict system-of-record boundary: QIHSE = truth; KEYSTONE = acceleration; CITADEL = policy & hypervisor execution. Snapshot bootstrap and live stream handoff. |
| [keystoned.md](architecture/keystoned.md) | Standalone unprivileged service daemon, Unix domain socket architecture (`0700`), compact binary IPC framing with CRC32, and client library integration. |

---

## Benchmark records

The repository also retains historical and phase-specific benchmark reports:

- [benchmark_report.md](benchmark_report.md)
- [benchmark_report_phase5.md](benchmark_report_phase5.md)

Treat host-specific benchmark reports as measurements of the stated machine/build/workload, not as universal performance guarantees.

---

## Documentation rule

The root [README.md](../README.md) answers:

1. What does KEYSTONE do?
2. Why would an organization use it?
3. Where does it fit into existing infrastructure?
4. What has actually been measured or implemented?
5. Where can a technical reader go deeper?

Detailed backend mechanics, build switches, benchmark methodology, feature matrices, and experimental implementation notes belong under `docs/` so the public entry point stays readable to both technical and non-technical reviewers.
