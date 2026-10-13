"""Tests for KeystoneAnalysisIndex authorized projection, backreferences, and low-clearance denial."""

from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
import uuid

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from keystone_qihse_feed import TOMBSTONE
from keystone_qihse_feed.durable import DurableFeed
from keystone_qihse_feed.index import KeystoneAnalysisIndex, SearchPrincipal


def item(kind, value):
    return SimpleNamespace(kind=kind, value=value)


def envelope(offset=0, *, event=1, obj=200, generation=1, flags=0, payload=b"manifest",
             clearance=2, sci=1, resource="suite/artifact/sha384_sample"):
    ident = lambda n: str(uuid.UUID(int=n)).encode("ascii")
    return SimpleNamespace(kind="array", items=[
        item("int", offset), item("bulk", b"suite.manifest"),
        item("bulk", resource.encode("ascii")), item("int", clearance),
        item("int", sci), item("int", 77), item("int", generation),
        item("bulk", payload), item("bulk", ident(event)),
        item("bulk", ident(100)), item("bulk", ident(obj)),
        item("int", 1), item("int", 1000 + event), item("int", 0),
        item("int", flags), item("int", 42)])


class MockController:
    def __init__(self, replies=()):
        self.replies = iter(replies)
        self.calls = []

    def keystone_feed_open(self, **kwargs):
        self.calls.append(("OPEN", kwargs))
        return SimpleNamespace(feed_id=1)

    def call(self, *args):
        self.calls.append(args)
        if args[0] == "KEYSTONE.FEED.NEXT":
            return next(self.replies)
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

            # 1. Low clearance principal cannot see high-data
            low_p = SearchPrincipal(tenant_id=77, clearance=1, sci=1)
            hits_low = idx.search(low_p, "artifact")
            self.assertEqual(len(hits_low), 1)
            self.assertEqual(hits_low[0].classification, 1)

            # 2. High clearance principal can see both
            high_p = SearchPrincipal(tenant_id=77, clearance=3, sci=1)
            hits_high = idx.search(high_p, "artifact")
            self.assertEqual(len(hits_high), 2)

            # 3. Wrong tenant principal gets zero hits
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
            self.assertEqual(len(active_hits), 0) # Overwritten by tombstone

            all_hits = idx.search(p, "item", include_tombstones=True)
            self.assertEqual(len(all_hits), 1)

            feed.close()
