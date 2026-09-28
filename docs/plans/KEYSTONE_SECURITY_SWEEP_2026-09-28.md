# KEYSTONE Full Security Sweep — 2026-09-28

Defensive code-security sweep of `src/` + `include/` against the CITADEL hostile-input
doctrine (magic/version, explicit lengths, checked arithmetic, allocation bounds before
read, negative-authorization tests for every externally reachable surface). Review-only;
nothing was modified during the sweep. Remediation status tracked in §3.

Priorities set by this sweep reshape the roadmap order: loader memory-safety first
(#1–3), then the keystoned trust model (#4–6), then label filtering (#7–8), then parity
items. **All five tiers were remediated on 2026-09-28** — see §3.

---

## 1. Findings

### CRITICAL

| # | Location | Finding | Exploit shape |
|---|----------|---------|---------------|
| 1 | `src/keystone_trigram.c:2230-2236` (V2 loader) | Posting-list `count`/`offset` from file never validated | Crafted `.tgi`: `meta.count`/`meta.offset` set `doc_ids = flat_postings + offset`, `count` verbatim — no `offset+count <= total_postings`; `total_postings==0` yields `NULL + offset` wild pointers. |
| 2 | `src/keystone_trigram.c:2266-2271` | OOB **write** during load from unvalidated doc-ids | Dense-bitmap build does `bm[did>>6] |= 1<<(did&63)` with `did` read from the hostile posting slice → bit-set primitive up to ~512MB past an 8-byte chunk, inside `load()` itself. |
| 3 | `src/keystone_trigram.c:1235, 1260-1288, 1912-1919` | OOB **read** at query time from hostile posting lists | Single-list fast path memcpy's `count*4` bytes into caller candidates (heap disclosure); bitmap probe and iterator read past `flat_postings`; `search()` re-checks `doc_id < doc_count` only *after* the OOB read. |

### HIGH

| # | Location | Finding |
|---|----------|---------|
| 4 | `src/service/keystoned_server.c:299-308,327,359,381` | IPC security context is fully client-asserted; no `SO_PEERCRED`/uid/token — any same-uid process claims `TOP_SECRET + ~0` compartments; `hdr.reserved` never validated. |
| 5 | `src/service/keystoned_server.c:182,221` | Compartments hardcoded to 0 (dead code); tenant never compared — cross-tenant reads for any cleared caller. |
| 6 | `include/keystone_federation.h:46`; only use `src/temporal/keystone_temporal_index.c:316` | `SECURITY_SENSITIVE` never enforced at query time (only counted as bucket `error_count`). Should gate: keystoned exact+temporal, temporal range/object/reverse, exact lookup, RAG event/neighbor/incident collection, both federated merges, ingest trust evaluation. |
| 7 | `src/query/keystone_rag_engine.c:104-149` | RAG: `caller_security == NULL` skips authz entirely; per-item classification of events/neighbors/anomalies/incidents never checked — SECRET events on an UNCLASSIFIED-marked object reach an OPS caller. |
| 8 | `src/federation/keystone_federated_query.c:161,177-183,237,257-263` | Merge `total_incoming`/pool sizing unchecked (`checked_*` unused); hostile node count → undersized pool → memcpy heap overflow; merged entries not label-filtered. |

### MEDIUM

| # | Location | Finding |
|---|----------|---------|
| 9 | `src/keystone_trigram.c:2197-2227` | Trigram loader allocation bomb: ~100-byte file declaring max buckets commits ~800MB+ before any read; no fstat size bound. |
| 10 | `src/keystone_trigram.c:2001-2095` | Trigram save: no CRC, no tmp+rename, no fsync (torn index on crash; edits undetectable). |
| 11 | `src/service/keystoned_server.c:208-224` | Temporal query OOB read for in-process `max_results > 1024` (filter loop reads `raw[i]` past the 1024-entry fetch buffer; IPC path clamps to 256). |
| 12 | `src/service/keystoned_server.c:440-454` | Single-threaded daemon, blocking `recv` without timeout — one held connection DoSes the service. |
| 13 | `src/federation/keystone_federation_ingest.c:674-687` | Watermark poisoning: one valid-CRC record with `fencing_epoch = UINT64_MAX` irreversibly bricks ingestion (all later records stale-rejected). |
| 14 | `src/keystone_tar_zst.c:318-328,1501-1539` | Sidecar `.idx.json`: no integrity check; `bloom_hashes` unbounded (~2^64-iteration CPU DoS per lookup); offsets/keys unvalidated. |
| 15 | `src/service/keystoned_server.c:23,460` | Default socket `/tmp/keystone.sock`: pre-created file blocks startup (DoS) + bind→chmod 0700 window. |

### LOW

| # | Location | Finding |
|---|----------|---------|
| 16 | `keystone_federation_ingest.c:358-408` | Checkpoint save/load below the loader bar (unchecked tmp-path snprintf, trailing garbage accepted, `reserved` outside CRC coverage). |
| 17 | `keystone_federation_ingest.c:41,289-339` | Wire envelope `reserved` not validated; nonzero payload-CRC accepted when `payload_len==0`. |
| 18 | ingest/exact/topology `next_pow2` + `tombstone_grow` | `next_power_of_two(SIZE_MAX-1)` wraps (trusted-config only); silent tombstone-table growth failure. |
| 19 | `keystone_temporal_index.c:289-297` | `aggregate_buckets` uint64 arithmetic can wrap (no memory-safety impact; clamped). |
| 20 | `keystone_federation_ingest.c:289-340` | Deserialized `payload` aliases the input buffer (documented borrow needed); trigram `stats` restored unvalidated. |

**Confirmed clean:** the hardened temporal + exact loaders meet the full bar (verified
against the corruption tests; `hdr.count*2` in exact load is file-size-bounded);
wire deserializer ordering is correct; ingest dedup backward-shift deletion is correct
under adversarial inserts; fabric datagram parsers, AVX search cores, the 64MiB direct
directory, topology/hybrid out-buffer discipline, and the QIHSE bridge fail-closed
behavior are clean. No `strcpy`/`sprintf`/`strcat`/`alloca` in `src/`.

## 2. Negative-authorization / negative-input test coverage

COVERED: temporal loader (10 cases), exact loader (10 cases), wire envelope tampering,
keystoned classification-denial matrix, bridge stub fail-closed.
PARTIAL: keystoned (no spoofed-sec_ctx, compartment, tenant, malformed-frame, oversized
payload tests); RAG (no NULL-sec_ctx or per-event leakage tests).
NOT COVERED: checkpoint, trigram loader (the most important gap given #1–3), tar_zst
sidecar, federated merges (hostile counts, cross-label).

## 3. Remediation status

| Priority | Items | Status |
|---|---|---|
| P0 — trigram loader memory safety (#1,2,3,9,10) | validation pass + save hardening + hostile-file tests | **FIXED** — `06e8b95` (fstat bounds, slice+doc-id validation, CRC trailer + atomic save, 8-case test matrix) |
| P1 — keystoned trust model (#4,5,11,12,15) | SO_PEERCRED auth, tenant checks, DoS hardening | **FIXED** — `c0ead0e` (same-uid peer auth, strict tenant isolation + tenant-bound ingest, SENSITIVE-at-SECRET, fetch-cap OOB, 10s recv timeout, strict frames, /run default socket; negative tests) |
| P2 — label enforcement (#6,7) | SECURITY_SENSITIVE gating, RAG per-item filtering + mandatory sec ctx | **FIXED** — `646c6a7` (RAG: mandatory authz, per-event classification/tenant/SENSITIVE filter, neighbor exclusion, events OOB fix; tests) |
| P3 — merge hardening (#8) | checked arithmetic in federated merges | **FIXED** — `322df76` (checked add/mul, fail-closed on hostile counts, test). Merge-level label filtering deferred to the federation-transport API work (merges are in-process calls over caller-owned arrays today) |
| P4 — parity items (#13,14,16,17,18,19,20) | checkpoint/envelope/tar_zst strictness, watermark bounds, table growth | **FIXED** — `acc5d20` (checkpoint reserved+exact-size+tests, envelope reserved/empty-CRC strictness, watermark caps + unbrick test, next_pow2 guards, tombstone-growth propagation, bloom-hash clamp, aggregation wrap guard, borrow documented) |

Residuals (accepted, tracked): compartments on keystoned queries remain structurally
absent because neither the exact/temporal entries nor the wire envelope carry a
compartment field — closing it needs the `sci` field added to the QIHSE envelope
(a cross-repo wire-version decision, flagged in the alignment audit §3); merge-output
label filtering lands with the federation transport. Anomalies/incidents carry no
per-item classification fields (scoped by target resource only).

Fixed during the same session (found by the new sanitizer matrix, not this sweep):
trigram serial-build merge crash (`83199e3`), uninitialized-UUID wire leak in the
keystoned test (`83199e3`), exact-loader parity (`c0aa72d`), temporal loader (`0ae7b83`).

*Sweep performed 2026-09-28 by the security review agent; report committed unmodified
from its findings.*