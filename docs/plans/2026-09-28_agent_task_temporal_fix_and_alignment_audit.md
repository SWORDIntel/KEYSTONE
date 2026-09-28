# KEYSTONE Agent Task — Temporal-Index Overflow Fix + Federation Alignment Audit

**Status:** queued task brief for an implementation-agent session · **Created:** 2026-09-28
**Authority:** operator request 2026-09-28; implements the KEYSTONE lane of CITADEL master-roadmap wave M1 (`../CITADEL/docs/ROADMAP.md` §6, M1).
**Audience:** a fresh agent session in this repository. This file is self-contained; read it fully before touching anything.

---

## 0. You are working in SWORDIntel/KEYSTONE

A native C11 indexing/intelligence engine. In the CITADEL architecture KEYSTONE is the **non-authoritative** intelligence layer over the QIHSE data plane. Governing rules, in precedence order:

1. This file.
2. `../CITADEL/AGENTS.md` (component-authority boundaries — §3 "KEYSTONE MUST NOT" applies to you).
3. `../CITADEL/docs/architecture/KEYSTONE_FEDERATION_INTELLIGENCE_UPGRADE_BRIEF.md` — the upgrade spec (55 sections, 16 acceptance criteria).
4. This repo's `README.md` / `ROADMAP.md`.

Use relative paths only (repo-relative, or `../QIHSE`, `../CITADEL`, `../INFRA/parrot-sabot` for siblings). Never write absolute home paths into code, tests, or docs.

## 1. State of the world (verified 2026-09-28)

- The federation-upgrade Phases 0–7 were **committed 2026-09-20** (`fd928de` → `8858574`; benchmark suite `482f948`; docs `a614bf8`): federation envelope + ingestion, exact/temporal indexes, `keystoned` service mode, topology cache + hybrid query planner, telemetry + incident similarity, federated query, RAG context packs.
- **Only one consumer path is verified live**: the audit-journal feed (R6b, 2026-09-26) at `../INFRA/parrot-sabot/scripts/keystone_journal_feed.py` — ctypes over `libkeystone` federation API, cursor resume, backfill/incremental/verify/synthetic all PASS against the production fleet.
- **OPEN BUG (Task 1):** `keystone_temporal_index_append` writes **8 bytes past the end of a loaded array** — valgrind-confirmed, found by the live feed under production load. Location: `src/temporal/keystone_temporal_index.c:117` (declaration `include/keystone_temporal.h:60`; design doc `docs/architecture/temporal_index.md`). Current production workaround: the feed rebuilds its index from spool. Nothing else is known-broken.
- **The working tree is dirty with the operator's in-flight Python-SDK move** (`python/keystone/*` deleted, `python/keystone_vectorsdk_disabled/` added): do **not** revert, complete, "clean up", or extend it, and keep it out of your commits.
- The operator's recorded expectation "KEYSTONE is fully aligned now" (in `../CITADEL/docs/decisions/ARCHITECTURE_DECISIONS.md`) is **unverified** — that is Task 2.

## 2. Task 1 — fix the temporal-index overflow

**Investigate first, then fix the invariant break, not the symptom.** The reported shape is an append past a *loaded* array, so the prime suspect is the load path trusting persisted counts/capacities (per CITADEL rule: persisted formats are hostile input even when CITADEL/qihse wrote them). If the loader accepts a corrupt count/capacity pair, that is a second bug — fix the loader too and harden persisted-format validation (explicit lengths, checked arithmetic, allocation bounds before any read).

Requirements, all mandatory:

1. **Reproducer test that fails pre-fix** — feed-shaped: build an index, persist, load, append beyond the loaded capacity; assert the old code overflows (ASan catch or explicit bounds assertion). Add to `tests/test_exact_temporal_index.c` (Makefile already builds it).
2. **Fix** in `src/temporal/keystone_temporal_index.c` (and the loader if implicated).
3. **Valgrind + ASan/UBSan clean** on the temporal and federation suites; add a malformed/truncated-persisted-input test if you touched the loader.
4. **Benchmark before/after** temporal append + temporal query using the existing `benchmarks/` harness; record the numbers in the audit doc (Task 2) — the fix must not silently regress the index path.

Note: retiring the feed's spool-rebuild workaround is an **operator rollout decision** (the deployed `libkeystone` must carry the fix first). Record it in the handoff section; do not modify parrot-sabot.

## 3. Task 2 — federation alignment audit ("fully aligned"?)

Produce `docs/plans/KEYSTONE_FEDERATION_ALIGNMENT_AUDIT_<date>.md` containing:

1. **Per-criterion verdict table** for all 16 acceptance criteria in the upgrade brief: `IMPLEMENTED+TESTED` / `COMMITTED-UNVERIFIED` / `PARTIAL` / `NOT-BUILT`, each with evidence (source file, test name, commit hash). Be strict: *committed ≠ verified*; nothing may be called "live-verified" without fleet evidence; the feed path is currently the only live-verified path.
2. **Benchmark run**: `benchmarks/bench_federation` + the federation suite (see `benchmarks/FEDERATION_BENCHMARK.md`) — locally first; then, where the harness supports it, **read-only against the live fleet**: t420 `192.168.1.91:7100` (brain), 730xd `192.168.1.90:7115`, t320 `192.168.1.250:7101`. The fleet is **production**: read-only queries and feed verification only — no daemon restarts, no writes beyond the feed's own cursor state, nothing destructive without explicit operator instruction.
3. **Wire-contract check against QIHSE**: the feed reply is the 16-item `KEYSTONE.FEED.NEXT` envelope (offset, type, resource, classif, sci, tenant, generation, payload, event_id, origin_node, object_id, fencing_epoch, hlc_physical, hlc_logical, **flags**, object_type) — see the QIHSE side (`../QIHSE/ROADMAP.md`, "KEYSTONE §4 envelope on the wire"). Verify KEYSTONE's ingest consumes the **full** envelope and honors `TOMBSTONE` and `SECURITY_SENSITIVE` flags (a dropped flags word is how deleted objects resurrect in an index).
4. **Status-line corrections**: where `docs/STATUS_SUMMARY.md`, `ROADMAP.md`, or `docs/architecture/*.md` claim more than the evidence supports, downgrade the claim in the same pass. Never upgrade a claim without new evidence.

## 4. Hard constraints (violation = stop and surface, do not proceed)

- **KEYSTONE is never authoritative**: no lease/ownership writes, no allocation decisions, no policy enforcement. Recommendations are not commands.
- **QIHSE access is read/index identity only**: tenant-scoped ANALYST, never OPERATOR (the feed contract). If you need new scopes, stop and surface.
- **Honest-status discipline**: never mark aspirational work implemented or verified.
- **Persisted and wire formats are hostile input**: magic/version, explicit lengths, checked arithmetic, allocation bounds, malformed/truncated-input tests for anything you touch.
- **Any new externally reachable surface ships a negative-authorization test.**
- **Commit discipline**: source + build wiring + test land in the same commit; conventional commit style per `git log`. Do not commit the operator's in-flight SDK move.
- One engine tree lane at a time: coordinate with the operator before touching anything `../QIHSE`.

## 5. Definition of done

- [ ] Reproducer (fails pre-fix) + fix + loader hardening if implicated
- [ ] Temporal + federation suites green under valgrind and ASan/UBSan
- [ ] Before/after benchmark numbers recorded
- [ ] `KEYSTONE_FEDERATION_ALIGNMENT_AUDIT_<date>.md` committed (per-criterion table + benchmark results + wire-contract check)
- [ ] Read-only fleet benchmark run recorded (or an explicit note of what blocked it)
- [ ] Overclaiming status lines corrected
- [ ] Handoff section for the operator: fix-rollout requirement to retire the feed workaround, plus anything discovered and deliberately not fixed

## 6. Suggested order

1. Read the brief's acceptance criteria and `docs/architecture/temporal_index.md`.
2. Write the failing reproducer; confirm it catches the overflow.
3. Fix + harden + sanitize + re-run suites.
4. Run local benchmarks; then the read-only fleet run.
5. Build the audit table criterion by criterion.
6. Correct status lines; write the handoff; commit in the repo's style.
