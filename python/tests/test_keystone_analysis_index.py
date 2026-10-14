"""Tests for KeystoneAnalysisIndex authorized candidate projection, stream consumer,
daemon security, and comprehensive resilience gates.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import sqlite3
import tempfile
from types import SimpleNamespace
import unittest
import uuid

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from keystone_qihse_feed import TOMBSTONE
from keystone_qihse_feed.durable import DurableFeed
from keystone_qihse_feed.index import (
    KeystoneAnalysisIndex,
    KeystoneFeedConsumer,
    PayloadIntegrityError,
    QihseOutageError,
    QihseReauthorizationError,
    SearchAuthorizationError,
    SearchPrincipal,
    retrieve_artifact_payload,
)


def item(kind, value):
    return SimpleNamespace(kind=kind, value=value)


def envelope(
    offset=0,
    *,
    event=1,
    obj=200,
    generation=1,
    flags=0,
    payload=b"manifest",
    clearance=2,
    sci=1,
    resource="suite/artifact/sha384_sample",
):
    ident = lambda n: str(uuid.UUID(int=n)).encode("ascii")
    return SimpleNamespace(
        kind="array",
        items=[
            item("int", offset),
            item("bulk", b"suite.manifest"),
            item("bulk", resource.encode("ascii")),
            item("int", clearance),
            item("int", sci),
            item("int", 77),
            item("int", generation),
            item("bulk", payload),
            item("bulk", ident(event)),
            item("bulk", ident(100)),
            item("bulk", ident(obj)),
            item("int", 1),
            item("int", 1000 + event),
            item("int", 0),
            item("int", flags),
            item("int", 42),
        ],
    )


class MockController:
    def __init__(self, replies=(), *, artifact_store=None, simulate_outage=False):
        self.replies = iter(replies)
        self.calls = []
        self.artifact_store = artifact_store or {}
        self.simulate_outage = simulate_outage

    def keystone_feed_open(self, **kwargs):
        self.calls.append(("OPEN", kwargs))
        return SimpleNamespace(feed_id=1)

    def call(self, *args):
        self.calls.append(args)
        cmd = args[0]
        if cmd == "KEYSTONE.FEED.NEXT":
            return next(self.replies)
        if cmd == "QIHSE.ARTIFACT.GET":
            if self.simulate_outage:
                raise ConnectionResetError("QIHSE daemon connection dropped (simulated outage)")
            art_id = args[1]
            tenant_id = int(args[2])
            clearance = int(args[3])
            sci = int(args[4])
            if art_id in self.artifact_store:
                entry = self.artifact_store[art_id]
                if entry.get("tenant_id", tenant_id) != tenant_id:
                    return SimpleNamespace(kind="error", value=b"ERR_DENIED")
                if clearance < entry.get("classification", 0):
                    return SimpleNamespace(kind="error", value=b"ERR_DENIED")
                if entry.get("sci", 0) & ~sci:
                    return SimpleNamespace(kind="error", value=b"ERR_DENIED")
                return SimpleNamespace(kind="bulk", value=entry["payload"])
            return SimpleNamespace(kind="error", value=b"ERR_NOT_FOUND")
        return SimpleNamespace(kind="simple", value=b"OK")


class KeystoneAnalysisIndexTests(unittest.TestCase):
    def test_authorized_search_and_clearance_denial(self):
        with tempfile.TemporaryDirectory() as scratch:
            ctrl = MockController([
                envelope(0, event=1, obj=201, generation=1, clearance=3, sci=1,
                         resource="suite/artifact/high_secret_blob"),
                envelope(10, event=2, obj=202, generation=1, clearance=1, sci=1,
                         resource="suite/artifact/low_unclass_blob"),
                item("int", 0),
            ])
            feed = DurableFeed(ctrl, Path(scratch) / "feed", tenant_id=77,
                               clearance=3, sci=1, prefix="suite/")
            feed.open()
            self.assertIsNotNone(feed.next())
            self.assertIsNotNone(feed.next())
            self.assertIsNone(feed.next())

            idx = KeystoneAnalysisIndex(feed)
            synced = idx.sync_from_durable()
            self.assertEqual(synced, 2)

            # 1. Low clearance principal cannot see high-data (no metadata leak)
            low_p = SearchPrincipal(tenant_id=77, clearance=1, sci=1)
            hits_low = idx.search(low_p, "artifact")
            self.assertEqual(len(hits_low), 1)
            self.assertEqual(hits_low[0].classification, 1)

            # 2. High clearance principal can see both
            high_p = SearchPrincipal(tenant_id=77, clearance=3, sci=1)
            hits_high = idx.search(high_p, "artifact")
            self.assertEqual(len(hits_high), 2)

            # 3. Wrong tenant principal gets zero hits (no cross-tenant metadata leak)
            wrong_tenant_p = SearchPrincipal(tenant_id=99, clearance=3, sci=1)
            hits_wrong_tenant = idx.search(wrong_tenant_p, "artifact")
            self.assertEqual(len(hits_wrong_tenant), 0)

            # 4. Wrong SCI compartment gets zero hits
            wrong_sci_p = SearchPrincipal(tenant_id=77, clearance=3, sci=0)
            hits_wrong_sci = idx.search(wrong_sci_p, "artifact")
            self.assertEqual(len(hits_wrong_sci), 0)

            feed.close()

    def test_tombstone_exclusion(self):
        with tempfile.TemporaryDirectory() as scratch:
            ctrl = MockController([
                envelope(0, event=1, obj=205, generation=1, flags=0,
                         resource="suite/artifact/item_to_delete"),
                envelope(10, event=2, obj=205, generation=2, flags=TOMBSTONE,
                         resource="suite/artifact/item_to_delete"),
                item("int", 0),
            ])
            feed = DurableFeed(ctrl, Path(scratch) / "feed", tenant_id=77,
                               clearance=2, sci=1, prefix="suite/")
            feed.open()
            feed.next()
            feed.next()

            idx = KeystoneAnalysisIndex(feed)
            idx.sync_from_durable()

            p = SearchPrincipal(tenant_id=77, clearance=2, sci=1)
            active_hits = idx.search(p, "item", include_tombstones=False)
            self.assertEqual(len(active_hits), 0)  # Overwritten by tombstone

            all_hits = idx.search(p, "item", include_tombstones=True)
            self.assertEqual(len(all_hits), 1)
            self.assertEqual(all_hits[0].status, "tombstone")
            self.assertFalse(all_hits[0].incomplete)

            feed.close()

    def test_candidate_projections_disassembly_metadata_and_incomplete(self):
        with tempfile.TemporaryDirectory() as scratch:
            manifest_data = {
                "schema": "kp14.disassembly.v1",
                "artifact_id": "sha384_radare3_vexel_func_001",
                "disassembly": "mov rax, [rbx+0x20]\ntest rax, rax\njz loc_fail",
                "producer": "radare3",
                "fidelity": "partial",
                "completion": "partial",
                "verdict": "VULNERABLE",
                "functions": ["sym.vulnerable_handler", "sym.probe_sink"],
            }
            payload_bytes = json.dumps(manifest_data).encode("utf-8")

            ctrl = MockController([
                envelope(0, event=1, obj=210, generation=1, flags=0,
                         resource="suite/artifact/sha384_radare3_vexel_func_001",
                         payload=payload_bytes, clearance=2, sci=1),
                item("int", 0),
            ])
            feed = DurableFeed(ctrl, Path(scratch) / "feed", tenant_id=77,
                               clearance=2, sci=1, prefix="suite/")
            feed.open()
            feed.next()

            idx = KeystoneAnalysisIndex(feed)
            idx.sync_from_durable()

            p = SearchPrincipal(tenant_id=77, clearance=2, sci=1)

            # 1. Search by permitted disassembly text tokens
            hits_disasm = idx.search(p, "rbx+0x20")
            self.assertEqual(len(hits_disasm), 1)
            hit = hits_disasm[0]
            self.assertEqual(hit.source_artifact_id, "sha384_radare3_vexel_func_001")
            self.assertEqual(hit.qihse_artifact_id, "sha384_radare3_vexel_func_001")
            self.assertEqual(hit.status, "incomplete")
            self.assertTrue(hit.incomplete)
            self.assertFalse(hit.stale)
            self.assertEqual(hit.freshness, "fresh")
            self.assertEqual(hit.cursor, 0)
            self.assertEqual(hit.generation, 1)

            # 2. Search by metadata tokens
            hits_meta = idx.search(p, "vulnerable_handler")
            self.assertEqual(len(hits_meta), 1)

            # 3. Verify SQLite search_projection does NOT store raw payload bytes
            cursor = idx.db.cursor()
            cursor.execute("SELECT * FROM search_projection WHERE object_id = ?", (hit.object_id,))
            col_names = [d[0] for d in cursor.description]
            self.assertNotIn("payload", col_names)

            feed.close()

    def test_qihse_reauthorization_and_exact_content_verification(self):
        with tempfile.TemporaryDirectory() as scratch:
            raw_payload = b"int vulnerable() { return *ptr; }"
            expected_digest = hashlib.sha384(raw_payload).hexdigest()
            art_id = f"sha384_{expected_digest}"

            ctrl = MockController(
                [
                    envelope(0, event=1, obj=220, generation=1, clearance=2, sci=1,
                             resource=f"suite/artifact/{art_id}"),
                    item("int", 0),
                ],
                artifact_store={
                    art_id: {
                        "payload": raw_payload,
                        "tenant_id": 77,
                        "classification": 2,
                        "sci": 1,
                    }
                },
            )

            feed = DurableFeed(ctrl, Path(scratch) / "feed", tenant_id=77,
                               clearance=2, sci=1, prefix="suite/")
            feed.open()
            feed.next()
            idx = KeystoneAnalysisIndex(feed)
            idx.sync_from_durable()

            # Authorized principal retrieves payload with exact verification
            p_auth = SearchPrincipal(tenant_id=77, clearance=2, sci=1)
            hits = idx.search(p_auth, "artifact")
            self.assertEqual(len(hits), 1)

            bytes_out = retrieve_artifact_payload(
                p_auth, hits[0], ctrl, expected_content_digest=expected_digest
            )
            self.assertEqual(bytes_out, raw_payload)

            # Low clearance principal denied client-side (no metadata leak)
            p_low = SearchPrincipal(tenant_id=77, clearance=1, sci=1)
            with self.assertRaises(SearchAuthorizationError):
                retrieve_artifact_payload(p_low, hits[0], ctrl)

            # Wrong tenant denied client-side
            p_wrong_tenant = SearchPrincipal(tenant_id=99, clearance=2, sci=1)
            with self.assertRaises(SearchAuthorizationError):
                retrieve_artifact_payload(p_wrong_tenant, hits[0], ctrl)

            # Server-side denial at QIHSE
            ctrl.artifact_store[art_id]["classification"] = 3
            with self.assertRaises(QihseReauthorizationError):
                retrieve_artifact_payload(p_auth, hits[0], ctrl)

            feed.close()

    def test_tamper_rejection(self):
        with tempfile.TemporaryDirectory() as scratch:
            genuine_payload = b"original safe code bytes"
            genuine_digest = hashlib.sha384(genuine_payload).hexdigest()
            art_id = f"sha384_{genuine_digest}"

            tampered_payload = b"tampered altered injected bytes"

            ctrl = MockController(
                [
                    envelope(0, event=1, obj=230, generation=1, clearance=2, sci=1,
                             resource=f"suite/artifact/{art_id}"),
                    item("int", 0),
                ],
                artifact_store={
                    art_id: {
                        "payload": tampered_payload,  # Tampered bytes
                        "tenant_id": 77,
                        "classification": 2,
                        "sci": 1,
                    }
                },
            )

            feed = DurableFeed(ctrl, Path(scratch) / "feed", tenant_id=77,
                               clearance=2, sci=1, prefix="suite/")
            feed.open()
            feed.next()
            idx = KeystoneAnalysisIndex(feed)
            idx.sync_from_durable()

            p = SearchPrincipal(tenant_id=77, clearance=2, sci=1)
            hit = idx.search(p, "artifact")[0]

            # Retrieval fails closed upon digest mismatch
            with self.assertRaises(PayloadIntegrityError):
                retrieve_artifact_payload(p, hit, ctrl, expected_content_digest=genuine_digest)

            feed.close()

    def test_qihse_outage_fail_closed(self):
        with tempfile.TemporaryDirectory() as scratch:
            raw_payload = b"sample bytes"
            digest = hashlib.sha384(raw_payload).hexdigest()
            art_id = f"sha384_{digest}"

            ctrl = MockController(
                [
                    envelope(0, event=1, obj=240, generation=1, clearance=2, sci=1,
                             resource=f"suite/artifact/{art_id}"),
                    item("int", 0),
                ],
                artifact_store={art_id: {"payload": raw_payload, "tenant_id": 77, "classification": 2, "sci": 1}},
                simulate_outage=True,
            )

            feed = DurableFeed(ctrl, Path(scratch) / "feed", tenant_id=77,
                               clearance=2, sci=1, prefix="suite/")
            feed.open()
            feed.next()
            idx = KeystoneAnalysisIndex(feed)
            idx.sync_from_durable()

            p = SearchPrincipal(tenant_id=77, clearance=2, sci=1)
            hits = idx.search(p, "artifact")
            self.assertEqual(len(hits), 1)

            # QIHSE outage fails closed without corrupting the candidate projection
            with self.assertRaises(QihseOutageError):
                retrieve_artifact_payload(p, hits[0], ctrl)

            # Index remains queryable for candidates
            self.assertEqual(len(idx.search(p, "artifact")), 1)
            feed.close()

    def test_stream_consumer_at_least_once_and_replay_gaps(self):
        with tempfile.TemporaryDirectory() as scratch:
            ctrl = MockController([
                envelope(0, event=1, obj=250, generation=1, resource="suite/artifact/one"),
                envelope(0, event=1, obj=250, generation=1, resource="suite/artifact/one"),  # Duplicate / at-least-once
                envelope(2000, event=2, obj=251, generation=2, resource="suite/artifact/two"),  # Gap in offset
                item("int", 0),
            ])
            feed = DurableFeed(ctrl, Path(scratch) / "feed", tenant_id=77,
                               clearance=2, sci=1, prefix="suite/")
            feed.open()
            idx = KeystoneAnalysisIndex(feed)
            consumer = KeystoneFeedConsumer(feed, idx)

            rec1 = consumer.consume_next()
            self.assertIsNotNone(rec1)
            self.assertEqual(rec1.offset, 0)

            # Duplicate event consumed idempotently
            rec_dup = consumer.consume_next()
            self.assertIsNotNone(rec_dup)
            self.assertEqual(consumer.duplicate_count, 1)

            # Event after gap consumed, gap recorded
            rec_gap = consumer.consume_next()
            self.assertIsNotNone(rec_gap)
            self.assertEqual(len(consumer.replay_gaps), 1)
            self.assertEqual(consumer.replay_gaps[0], (0, 2000))

            p = SearchPrincipal(tenant_id=77, clearance=2, sci=1)
            hits = idx.search(p, "artifact")
            self.assertEqual(len(hits), 2)

            feed.close()

    def test_rebuild_from_authority_equivalence_and_zero_mutation(self):
        with tempfile.TemporaryDirectory() as scratch:
            events = [
                envelope(0, event=1, obj=301, generation=1, resource="suite/artifact/a"),
                envelope(100, event=2, obj=302, generation=1, resource="suite/artifact/b"),
                envelope(200, event=3, obj=301, generation=2, flags=TOMBSTONE, resource="suite/artifact/a"),
                item("int", 0),
            ]

            ctrl1 = MockController(events)
            feed_dir = Path(scratch) / "feed_initial"
            feed1 = DurableFeed(ctrl1, feed_dir, tenant_id=77, clearance=2, sci=1, prefix="suite/")
            feed1.open()
            idx1 = KeystoneAnalysisIndex(feed1)
            consumer1 = KeystoneFeedConsumer(feed1, idx1)
            consumer1.consume_all()

            p = SearchPrincipal(tenant_id=77, clearance=2, sci=1)
            orig_active = idx1.search(p, "artifact", include_tombstones=False)
            orig_all = idx1.search(p, "artifact", include_tombstones=True)
            self.assertEqual(len(orig_active), 1)
            self.assertEqual(len(orig_all), 2)
            feed1.close()

            # Now rebuild from authority in a fresh directory
            ctrl2 = MockController(events)
            rebuild_dir = Path(scratch) / "feed_rebuilt"
            rebuilt_idx = KeystoneFeedConsumer.rebuild_from_authority(
                ctrl2, rebuild_dir, tenant_id=77, clearance=2, sci=1, prefix="suite/"
            )

            rebuilt_active = rebuilt_idx.search(p, "artifact", include_tombstones=False)
            rebuilt_all = rebuilt_idx.search(p, "artifact", include_tombstones=True)

            # Verify exact equivalence
            self.assertEqual(len(rebuilt_active), len(orig_active))
            self.assertEqual(rebuilt_active[0].object_id, orig_active[0].object_id)
            self.assertEqual(len(rebuilt_all), len(orig_all))

            # Verify QIHSE authority was never mutated
            mutation_ops = [c[0] for c in ctrl2.calls if "WRITE" in c[0] or "PUT" in c[0] or "DELETE" in c[0] or "UPDATE" in c[0]]
            self.assertEqual(mutation_ops, [])
