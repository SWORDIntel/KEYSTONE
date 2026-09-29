# KEYSTONE Federation Alignment Audit — 2026-09-28

**Scope:** Verdict on the operator's recorded expectation "KEYSTONE is fully aligned now"
(`../CITADEL/docs/decisions/ARCHITECTURE_DECISIONS.md`) against the 16 acceptance criteria of
[`../CITADEL/docs/architecture/KEYSTONE_FEDERATION_INTELLIGENCE_UPGRADE_BRIEF.md`](../../../CITADEL/docs/architecture/KEYSTONE_FEDERATION_INTELLIGENCE_UPGRADE_BRIEF.md) §51,
plus the temporal-index overflow fix (Task 1) and a read-only live-fleet verification run.

**Headline verdict (re-scored 2026-09-29): substantially aligned; "fully aligned" remains premature.**
Phases 0–7 were implemented and locally tested on 2026-09-20 (`fd928de` → `8858574`); the original
scoring below was **5 of 16 fully met, 10 partial, 1 structural** with one live-verified consumer.
The 2026-09-28/29 remediation program (security sweep fixes, then the roadmap lanes) moved the
score to **9 of 16 fully met** (2, 4, 5, 10, 11, 12, 13, 14, 15) and **substantially improved 4
more** (1, 6, 7, 16 — see the per-row updates). The remaining gaps are precisely: criterion 3
(controller operability evidence, CITADEL-side), **criterion 8** (compartment enforcement —
blocked on the QIHSE `sci` envelope field, a cross-repo wire decision), and **criterion 9**
(exact/trigram filters not yet in the hybrid planner). The fleet library is deployed current
through the remediation. The original verdict is retained below unchanged for the record.

---

## Original verdict (2026-09-28, pre-remediation)

**Headline verdict: NOT "fully aligned".** Phases 0–7 are **implemented and locally tested**
(commits `fd928de` → `8858574`, 2026-09-20), but the honest status is:
**5 of 16 criteria fully met, 10 partial, 1 not applicable to KEYSTONE alone** — and exactly
**one consumer path is live-verified** (the R6b audit-journal feed, 2026-09-26). Nothing else
has fleet evidence. A production heap-overflow bug in the temporal index loader (found by that
live feed) sat inside "Phase 1 — COMPLETED" for eight days; it is fixed in this change set.

---

## 1. Per-criterion verdict table

Verdicts: `IMPLEMENTED+TESTED` (code + passing tests in-repo) · `COMMITTED-UNVERIFIED`
(code merged, no test) · `PARTIAL` (built but incomplete vs. the brief) · `NOT-BUILT`.
"live-verified" is reserved for the feed path with fleet evidence.

| # | Criterion (brief §51) | Verdict | Evidence |
|---|---|---|---|
| 1 | Rebuild entirely from authoritative sources | **PARTIAL** (improved 2026-09-29: the feed's spool-rebuild pattern is now a first-class in-repo test — indexes destroyed, rebuilt from CRC-validated envelopes + checkpoint, proven equivalent to pre-loss state incl. tombstone/no-resurrection semantics; still open: trigram/vector families, rebuild from QIHSE itself rather than the local spool) | Feed rebuilds exact+temporal indexes from spool envelopes (`../INFRA/parrot-sabot/scripts/keystone_journal_feed.py` `rebuild_temporal`/`prune_spool`) — **live-verified 2026-09-26**. No in-repo tested rebuild-from-QIHSE workflow (brief §46, §53.15); trigram/vector families outside the federation lane have none. |
| 2 | QIHSE remains authoritative | **IMPLEMENTED+TESTED** (structural) | No lease/ownership/allocation/policy code anywhere in `src/`; all outputs are recommendation objects (`include/keystone_federation.h:188-195`). Tests assert advisory-only output: `test_explainable_recommendations` (`tests/test_federation_envelope.c`), non-authoritative incident bundles (`tests/test_telemetry_incident_engine.c`). QIHSE side enforces read/index identity (tenant-scoped ANALYST, `FEDERATION_READ` only — `../QIHSE/include/qihse_keystone.h:72-90`). |
| 3 | Controller operable if KEYSTONE unavailable | **PARTIAL** (by construction) | KEYSTONE is optional in every integration (bridge compiles fail-closed stubs without `KEYSTONE_ENABLE_QIHSE_BRIDGE`, `src/qihse_keystone_bridge.c`); no in-repo test demonstrates controller operation with KEYSTONE down — that evidence lives in CITADEL, not here. |
| 4 | Snapshot + live ingestion, no silent gaps | **IMPLEMENTED+TESTED (library level, 2026-09-29)** | Live polling stream with resumable cursor + idempotent re-apply: implemented and **live-verified** (feed cursor, 2026-09-26). Snapshot bootstrap mode + exact snapshot/live cursor handoff now BUILT (`keystone_bootstrap.h`): snapshot format at the full hostile-input bar (CRC, exact-size bound, atomic publish), bootstrap session with the exact-cursor handoff rule — snapshot-covered events detected as DUPLICATE and never double-applied, live-era events flow the normal ingest path, stale fencing still fenced, rolling snapshots supported; equivalence test proves snapshot+replay == all-live; format included in the fuzz corpus. Remaining for end-to-end: wiring a QIHSE snapshot source + the feed cursor to it. Engine checkpoint: as before. |
| 5 | Duplicate events idempotent | **IMPLEMENTED+TESTED** | Dedup ring + backward-shift deletion: `src/federation/keystone_federation_ingest.c` (`dedup_*`, `keystone_federation_ingest_submit` step 2); `test_ingestion_pipeline`; benchmark "100% duplicate rejection" (`benchmarks/FEDERATION_BENCHMARK.md` §1); feed accepts `INGEST_DUPLICATE` on re-feed. |
| 6 | Tombstones remove objects from all query families | **PARTIAL** (improved 2026-09-28 PM: temporal family now ships tombstone-aware active-history queries with as-of semantics and persistence-surviving deletion state, tested; engine registry + exact transitions already tested. Still open: trigram/vector families see no federation tombstones, live feed never emits them) | Engine tombstone registry + `keystone_federation_is_tombstoned` (tested via envelope suite); exact-index tombstone transitions tested (`tests/test_exact_temporal_index.c` `test_exact_identity_lifecycle` steps 7–8). BUT: temporal query APIs return the tombstone event itself (by design for timelines) with no tombstone-filtered object query; trigram/vector families receive no federation tombstones at all; the live feed never emits tombstones (append-only journal). Cross-family "no resurrection" is untested. |
| 7 | Every query result exposes freshness + index generation | **PARTIAL** (improved 2026-09-28 PM: `keystone_hlc_staleness_ms` + `keystone_temporal_index_watermark` give every family the same computed-staleness contract, tested; RAG packs and keystoned already carry generation/staleness. Still open: raw exact-index lookups expose per-entry generation/HLC but no computed staleness field) | RAG packs carry `index_generation`, `freshness_hlc`, `staleness_ms` (`include/keystone_rag.h:59-61`, tested); hybrid recommendations carry generation/HLC provenance; keystoned responses carry `index_generation` (`include/keystoned.h:67`). Raw exact/temporal query APIs return per-entry `source_generation`/HLC but no computed staleness; not uniform across families. |
| 8 | Sensitive classifications cannot leak via indexes/caches | **PARTIAL** | Query-time MAC exists and is **negatively tested**: keystoned denies under-clearance lookups (`tests/test_keystoned_service.c:92-98`, `KEYSTONED_STATUS_DENIED`), RAG enforces clearance + compartment masks (`src/query/keystone_rag_engine.c:105-109`, tested), hybrid Tier-1 prunes on classification. BUT: no physical/logical partitioning (brief §26 — single shared indexes); `SECURITY_SENSITIVE` flag is counted (`keystone_temporal_index_aggregate_buckets`) but **never enforced at query time**; QIHSE's `sci` field does not cross the envelope (§3 below). |
| 9 | Hybrid search combines exact/topology/temporal/vector filters | **PARTIAL** | Hybrid planner = topology + telemetry + security + ISA/RAM constraints (`src/query/keystone_hybrid_planner.c`, tested `test_hybrid_query_planner`); RAG = topology + temporal + telemetry + incident pillars (tested). Full §13 combination incl. trigram and exact-index in one planner: **NOT-BUILT**. |
| 10 | Placement output is a recommendation with hard-constraint evidence, never a command | **IMPLEMENTED+TESTED** | `keystone_recommendation_t` + `keystone_explain_t` with per-constraint pass/fail (`include/keystone_federation.h:170-195`); hybrid planner emits them (`test_hybrid_query_planner`); no execution path exists in the codebase. |
| 11 | Accelerator failures fall back to validated CPU paths | **IMPLEMENTED+TESTED** | Long-standing core property: scalar reference cross-validation (`test_core_native`, `test_auto_backend`), CUDA/AMX fallback contracts (`test_cuda_backend`), incident-engine cross-backend bit accuracy (`test_incident_similarity_and_silicon_dispatch`). |
| 12 | Persistent indexes reject malformed/corrupt data safely | **IMPLEMENTED+TESTED as of this change** (was **PARTIAL**) | Wire envelope hostile-input rejection tested (`test_wire_serialization`); checkpoint + exact-index CRC load paths tested. The temporal loader was **unsafe until today** (capacity/count invariant break → heap overflow; no file-size bound): fixed in this change set with a 10-case corruption/truncation/lying-header test (`test_temporal_persistence_rejects_corrupt`). Remaining gap vs. brief §31: per-loader ad-hoc validation, no shared hardened-decoding framework, no fuzz corpus (see criterion 16). |
| 13 | Index generation publication is atomic | **IMPLEMENTED+TESTED (durable layer, 2026-09-29)** — `keystone_manifest.h`: per-family manifests (§29: index_uuid, generation, source cursor, HLC bounds, record/tombstone counts, build+algorithm versions, payload CRC32) at the full hostile-input bar, plus the durable §30 lifecycle: publish = verify candidates through the real loaders → durable copies (tmp+fsync+rename) → manifests → ACTIVE pointer flipped LAST; open_latest serves the active generation, never one newer than the pointer, walks descending on corruption, and quarantines invalid candidates; failed publishes never touch the active pointer. In-memory reader switch (keystoned) remains the serving layer above. Tested: two-generation publish/serve, payload-tamper fallback + quarantine, failed-publish isolation | keystoned reader-writer pointer swap `keystoned_server_publish_generation` (tested); all persistence is tmp+fsync+rename atomic. Full §30 lifecycle (build → verify → manifest publish → retire) and `index_uuid` manifests: **NOT-BUILT**. |
| 14 | Distributed query tolerates partial KEYSTONE failure | **IMPLEMENTED+TESTED (transport added 2026-09-29)** — the merge core now has a real wire transport (`keystone_fed_transport.h`): CRC-checked framing, bounded label-filtering responders, fan-out with per-node timeouts and OK/UNREACHABLE/TIMEOUT/PROTOCOL statuses; tested with healthy/dead/silent nodes, cross-node dedup, both-end label enforcement, exact conflict resolution, and hostile-frame drops. Remaining for full production use: deployment + TLS/auth on the wire | Merge semantics implemented + tested: `keystone_partial_status_t`, timeline/top-K merges with node-health tracking (`src/federation/keystone_federated_query.c`, `test_federated_timeline_and_topk_merging`). There is **no network transport** — "nodes" are in-process arrays; real fan-out/timeout over sockets: **NOT-BUILT**. |
| 15 | Existing correctness/benchmark tests remain green | **IMPLEMENTED+TESTED** (with a caveat) | `make check` = 18/18 binaries green post-fix (2026-09-28, this host). Caveat: `tests/test_exact_temporal_index.c` asserts `ns_per_query < 500` and is borderline on loaded hosts (observed 477–498 ns passing, occasional fail) — pre-existing, environment-dependent, **not** related to the fix; left untouched deliberately. |
| 16 | New sanitizer/fuzz/stream/federation tests pass | **PARTIAL** (improved 2026-09-28 PM: stream edge battery added — duplicate, out-of-order generations, tombstone-before-create, rollback fencing, restart/replay with persisted fencing watermarks, gap tolerance; sanitizer runs are now `make asan`/`make valgrind`. deterministic persistence-fuzz corpus added 2026-09-29 (`tests/persistence_fuzz/`, 4 formats × 648 mutations, wired into check + both sanitizer targets. Still missing: cursor-gap reconciliation) | Sanitizers: temporal + all four federation suites pass ASan/UBSan and valgrind **in this audit** (first recorded run — not wired as Makefile/CI targets). Fuzz: **NOT-BUILT** (`tests/persistence_fuzz/` from the brief does not exist). Stream edge cases (cursor rollback, gap detection, restart mid-ingest): **NOT-BUILT**. `tests/federation/` exists but is empty; federation coverage lives in `tests/test_*.c` binaries. |

**Score: 5 IMPLEMENTED+TESTED (2, 5, 10, 11, 12*, 15), 10 PARTIAL, 1 structural (3).**
The pattern: per-phase engines and their unit tests are real; the *integration* properties the
brief actually gates on — cross-family tombstone removal, uniform freshness, physical security
partitioning, real distributed transport, fuzzing, offline rebuild as a tested workflow — are
the unfinished half. "Fully aligned" overstates the state by exactly that half.

---

## 2. Task 1 — temporal-index overflow fix (summary; full detail in commit)

**Bug.** `keystone_temporal_index_load` claimed `capacity = 16` for any loaded `count ≤ 16`
while allocating exactly `count` × 80-byte entries. `keystone_temporal_index_append` trusts
`count < capacity` and wrote `entries[count]` past the allocation — an 80-byte entry write
beginning exactly at the end of the block (valgrind's "invalid write of size 8" signature),
reproduced under ASan at `src/temporal/keystone_temporal_index.c:138` writing past the
400-byte region from `keystone_temporal_index_load:408`. Secondary bug in the same function:
a zero-entry file loaded `entries = NULL` with `capacity = 16` — first append was a NULL write.

**Fix** (`src/temporal/keystone_temporal_index.c`):
- Loader: strict magic/version/`reserved==0` checks; `checked_mul`/`checked_add` on declared
  counts; **exact file-size bound (`fstat`) before any allocation** — truncated, trailing-garbage,
  and lying-count files are rejected before memory is committed; allocation is for *capacity*
  slots (`max(count, 16)`), never bare `count`, so the array handed to `append` always has the
  capacity it claims; CRC scheme and on-disk format **unchanged** (production spool files remain
  loadable).
- `append`: doubling-overflow and zero-capacity guards on growth.
- `save`: reject path truncation of the tmp-file name instead of writing to a wrong path.

**Verification.**
- Reproducer `test_temporal_load_then_append_resume` (feed-shaped: build 5 → save → load →
  append through 21 → second resume cycle) fails pre-fix with the exact production ASan
  signature; passes post-fix.
- `test_temporal_empty_persist_resume`, `test_temporal_persistence_rejects_corrupt`
  (10 hostile-file cases incl. a CRC-valid count-lie and trailing-bytes case, both accepted pre-fix).
- ASan/UBSan: clean on `test_exact_temporal_index`, `test_federation_envelope`,
  `test_topology_hybrid_planner`, `test_telemetry_incident_engine`, `test_federated_query_rag`.
- Valgrind (`--leak-check=full`, `--error-exitcode`): clean on the four federation suites; the
  temporal suite shows zero invalid accesses (its process aborts on the pre-existing timing
  assertion under valgrind slowdown, so a standalone driver exercised the changed surface
  — create/append/save/load/resume-append/query/corrupt-reject/destroy — to a clean exit).
- `make check`: 18/18 green.

**Benchmark before/after** (`benchmarks/bench_federation`, Benchmark 3, 3 runs each, this host —
PVE container, loaded; medians shown; committed `FEDERATION_BENCHMARK.md` figures from the
2026-09-20 host are included for context):

| Metric (200k-entry timeline) | Before (pre-fix) | After (fixed) | Committed doc (Sep 20 host) |
|---|---|---|---|
| Append throughput | 9.79 M/s (102.1 ns) | 11.56 M/s (86.5 ns) | not measured previously |
| Range-query throughput | 0.88 M/s | 0.94 M/s | 1.39 M/s |
| Range-query p50 | 937 ns | 920 ns | 574 ns |
| Save 200k entries | 129.6 ms | 130.7 ms | — |
| Load 200k entries | 132.9 ms | 134.1 ms | — |
| Resume-appends (feed pattern) | 0.72 M/s | 0.72 M/s | — |

No regression: the deltas are inside this host's run-to-run noise (append 8.6–11.7 M/s across
runs both sides; query 0.74–0.98 M/s). The fix touches only the load path (one `fstat` +
capacity-based `calloc`); append/query machine code is unchanged except unreachable guards.

---

## 3. Wire-contract check against QIHSE (16-item `KEYSTONE.FEED.NEXT` envelope)

QIHSE delivers (verified against `../QIHSE/include/qihse_keystone.h:97-131` and
`../QIHSE/ROADMAP.md` §W2.5): `[offset, type, resource, classif, sci, tenant, generation,
payload, event_id, origin_node, object_id, fencing_epoch, hlc_physical, hlc_logical, flags,
object_type]`, with a flag vocabulary (TOMBSTONE…DERIVED) that is **value-identical** to
KEYSTONE's `KEYSTONE_RECORD_FLAG_*` (`include/keystone_federation.h:41-48`).

Field-by-field against KEYSTONE's canonical envelope (`wire_record_header_t`,
`src/federation/keystone_federation_ingest.c:33-54`) and ingest record:

| QIHSE wire item | KEYSTONE consumption | Status |
|---|---|---|
| `offset` | `payload_offset` / `location_slot` parameter on `*_ingest_record` | carried by consumer API |
| `type` (journal event type, string) | not represented; only `object_type` (u32) is carried | **not carried** |
| `resource` (string key) | consumed at the bridge as `uuid5(resource)` → `source_object_id` | transformed, not stored raw |
| `classif` | `classification` (u32) | carried + enforced at query time (keystoned/RAG/planner) |
| `sci` (u16 compartments) | **no field exists** in `keystone_federation_record_t` | **DROPPED — gap** |
| `tenant` | `tenant_id` | carried |
| `generation` | `source_generation` | carried, staleness-checked |
| `payload` | `payload`/`payload_len` + CRC | carried + CRC-verified |
| `event_id` | `source_event_id` | carried (dedup key) |
| `origin_node` | `source_node_id` | carried |
| `object_id` | `source_object_id` | carried |
| `fencing_epoch` | `fencing_epoch` | carried, stale-epoch rejection |
| `hlc_physical` / `hlc_logical` | `source_hlc` | carried |
| `flags` | `flags` (u32) — serialize/deserialize round-trip tested incl. `SECURITY_SENSITIVE` (`tests/test_federation_envelope.c:102,126`) | **carried — no dropped flags word in KEYSTONE's own envelope** |
| `object_type` | `object_type` (u32) | carried |

**Flag honoring.**
- `TOMBSTONE`: honored at ingest — tombstone registry insertion + `KEYSTONE_INGEST_TOMBSTONE_APPLIED`
  (`keystone_federation_ingest_submit` step 6), exact-index active↔tombstone transitions tested.
  A deleted object therefore cannot resurrect through the exact index or `is_tombstoned`.
  Residual gap: temporal object-scoped queries return the deletion event by design and no
  query API filters tombstoned objects out of results (criterion 6).
- `SECURITY_SENSITIVE`: **propagated but not enforced** — it survives the wire round-trip and is
  stored on entries, but the only reader is `keystone_temporal_index_aggregate_buckets`
  (counted as `error_count`). No query path denies or filters on it. This is the weakest link
  in the classification-propagation chain (criterion 8).

**Consumer reality.** No KEYSTONE-side code issues `KEYSTONE.FEED.NEXT` (grep over `src/`,
`include/` — zero hits). The RESP command exists and is tested on the QIHSE side
(`../QIHSE/tests/test_keystone_feed_w25.c`); the only live consumer of QIHSE data is the
parrot-sabot journal feed, which reads `audit/journal/*` keys directly via GET and constructs
records itself (flags always `FLAG_AUDIT`, never TOMBSTONE — the journal is append-only).
So even the live-verified path does not yet exercise the 16-item wire command.

---

## 4. Read-only live-fleet run (2026-09-28)

Harness: `../INFRA/parrot-sabot/scripts/keystone_journal_feed.py` (`--list`, `--verify` —
both read-only: SCAN/GET only; no cursor, counter, spool, or cluster writes).
`benchmarks/bench_federation` has no fleet mode — it is a local in-process benchmark; noted
as a harness limitation.

- **Nodes:** t420 brain `192.168.1.91:7100` reachable; t320 `192.168.1.250:7101` reachable;
  730xd `192.168.1.90:7115` **unreachable** during the run ("seed unreachable, key discovery
  incomplete" — scan fan-out for that node's keys was not performed; pre-existing condition,
  not caused by this audit).
- **`--list`:** 4 journal index records live (`audit/journal/sabot/t420/{audit_log,
  go_audit_log, integrity_chain, go_integrity_chain}`); engine checkpoint `ingested=6 gen=2
  epoch=1` loads OK; exact index 4 objects resumes OK; temporal rebuilt from spool (the
  workaround). Cursor/counter keys absent (pruned after the 9/26 synthetic test). One leftover
  synthetic batch key `audit/journal/sabot/test-host/synthetic_probe/1` remains in the cluster
  (index key was deleted, batch key not) — harmless, flagged for cleanup.
- **`--verify 4` (all items):** 3/4 PASS (envelope CRC ok, exact-index generation matches,
  tombstone=false, payload byte-identical to cluster). 1 FAIL:
  `audit_log seq=2` — spooled payload ≠ live cluster batch bytes **at the same generation**
  (live index-record digest matches the live batch, so the source rewrote batch content under
  an unchanged seq after it was fed). This is same-generation content drift on the QIHSE/journal
  side, correctly *detected* by the verifier's byte comparison; generation-based staleness alone
  cannot see it. Flagged to the operator; not a KEYSTONE defect.
- **Forensic confirmation of Task 1:** the production spool's `temporal.index` is 376 bytes =
  header + **4 entries** — squarely inside the ≤16 overflow window. The deployed
  `/mnt/external-nvme/INFRA/KEYSTONE/libkeystone.so` predates the fix; the feed's
  rebuild workaround remains load-bearing until rollout (§6).

---

## 5. Status-line corrections made in this pass

All downgrades, no upgrades (files edited in the same change set):

- `docs/STATUS_SUMMARY.md` — test table corrected to the 18 real `make check` binaries
  (previous table listed 19 with 7 that the Makefile does not build); Phase 1 "64-byte
  cache-line aligned" temporal entry corrected to the actual 80-byte `keystone_temporal_entry_t`;
  stale per-subsystem throughput figures replaced with pointers to
  `benchmarks/FEDERATION_BENCHMARK.md` + this audit (host-dependent); live-verification status
  subsection added; temporal-loader fix recorded.
- `ROADMAP.md` — Phase 1 temporal bullet: overflow fix + hostile-input loader hardening noted;
  benchmark numbers annotated with measurement context.
- `README.md` — badge "Phases 0–7 Complete" → "Implemented"; "fully implemented with 19/19
  passing test suites" → 18/18 with local-vs-live distinction; 64-byte entry claim fixed;
  measured-results section re-anchored to the benchmark docs.
- `docs/architecture/temporal_index.md` — entry layout corrected to the real 80-byte struct
  (previous doc specified a 64-byte struct that does not exist and an index struct with
  `is_sorted`/rwlock fields that are not implemented); §7 load-time validation description now
  matches the hardened implementation (it described bounds-validation that did not exist before
  this fix).

---

## 6. Handoff to the operator

1. **Fix rollout retires the feed workaround — EXECUTED 2026-09-28 (later the same day).**
   The fixed `libkeystone.so` (commit `0ae7b83`, SHA-256
   `e4801b25c7e61756e45140c2a1e5ea763304e69494cea5f1ca36b5ca69e49528`) was deployed
   atomically to `/mnt/external-nvme/INFRA/KEYSTONE/libkeystone.so` (feed/parrotagent
   primary) and `/opt/keystone/lib/libkeystone.so` (legacy fallback), each keeping a
   `.pre-0ae7b83.bak` rollback copy. Both deployments were functionally verified by
   loading the production 4-entry `temporal.index` spool and appending past the loaded
   count in-process (the exact pre-fix overflow path) — heap intact. The feed's
   `load_all` workaround was then retired (parrot-sabot `ce9cba9`): the temporal index
   now resumes like the exact index, with corrupt-spool fallback to the rebuild path;
   read-only fleet confirmation shows `temporal: resumed 4 entries (load ok)` and
   `--verify` matching the pre-rollout baseline (3/4; the `audit_log` drift item
   unchanged). The original guidance is retained below for the record.
   The deployed `libkeystone.so` (`/mnt/external-nvme/INFRA/KEYSTONE`, `/opt/keystone`)
   predates this fix at audit time. The production spool's temporal index (4 entries)
   is in the overflow window: **do not resume it on the old library.** After the fixed
   library is deployed, `load_all`'s temporal-rebuild workaround in
   `keystone_journal_feed.py` can be switched to a real `keystone_temporal_index_load`
   resume (the on-disk format is unchanged, so existing spool files load as-is).
   parrot-sabot was not modified during the audit pass, per instructions.
2. **Same-generation content drift on `audit_log`** (§4): the journal writer appears to have
   rewritten batch content under an unchanged `seq` after the feed captured it. The verifier
   catches it, but generation-only staleness cannot. Consider advancing `seq` (or versioning
   the digest) on any batch rewrite upstream.
3. **730xd (`192.168.1.90:7115`) was unreachable** during the audit run — pre-existing; fleet
   key discovery for that node is incomplete until it returns.
4. **Leftover cluster key** `audit/journal/sabot/test-host/synthetic_probe/1` (synthetic-test
   batch whose index key was deleted) — one `DEL` cleans it up.
5. **Discovered and deliberately not fixed** (out of scope this pass, recommend follow-ups):
   - `sci` (SCI compartments) from the QIHSE envelope has no field in
     `keystone_federation_record_t` — the spare/reserved word could carry it, but that is a
     wire-format decision needing a version bump on both sides.
   - `SECURITY_SENSITIVE` is never enforced at query time (only counted in temporal histograms).
   - Exact-index loader (`keystone_exact_index_load`) shares the save-path tmp-name truncation
     pattern and lacks the explicit file-size bound the temporal loader now has; it fails closed
     today (short `fread` + CRC), but should get the same `fstat` exact-length check for parity.
   - No KEYSTONE-side consumer of `KEYSTONE.FEED.NEXT` yet — when one is built (keystoned
     stream mode), it must carry `flags` and `sci` end-to-end (§3).
   - Sanitizer runs are not wired into the Makefile/CI (`asan`/`valgrind` targets); today's
     clean matrix is the first recorded one.
   - `ns_per_query < 500` timing assertion in `tests/test_exact_temporal_index.c` is
     borderline on loaded hosts — consider a CI-calibrated threshold or moving perf gates out
     of correctness suites (decision left to the operator; threshold not loosened here).
   - Brief §31's shared hardened-decoding framework and §54's `tests/security/`,
     `tests/persistence_fuzz/` remain unbuilt.

---

*Audit performed 2026-09-28 in `SWORDIntel/KEYSTONE` at `392f9bf` + this change set.
All fleet interactions were read-only (SCAN/GET) against the production cluster.*
