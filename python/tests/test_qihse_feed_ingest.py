"""Strict feed-envelope and non-serving staging regression tests."""

from dataclasses import replace
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
import uuid
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from keystone_qihse_feed import (  # noqa: E402
    FeedCorruption, FeedStager, SECURITY_SENSITIVE, TOMBSTONE, parse_next,
)


def item(kind, value):
    return SimpleNamespace(kind=kind, value=value)


def envelope(*, event=1, generation=1, flags=0, payload=b"manifest"):
    uuid = lambda n: str(__import__("uuid").UUID(int=n))
    values = [item("int", event), item("bulk", b"suite.manifest"),
              item("bulk", b"suite/artifact/1"), item("int", 2),
              item("int", 1), item("int", 77), item("int", generation),
              item("bulk", payload), item("bulk", uuid(event).encode()),
              item("bulk", uuid(100).encode()), item("bulk", uuid(200).encode()),
              item("int", 1), item("int", 1000 + event), item("int", 0),
              item("int", flags), item("int", 42)]
    return SimpleNamespace(kind="array", items=values)


class FakeController:
    def __init__(self, replies):
        self.replies = iter(replies)
        self.commands = []

    def keystone_feed_open(self, **kwargs):
        self.commands.append(("OPEN", kwargs))
        return SimpleNamespace(feed_id=9)

    def call(self, *args):
        self.commands.append(args)
        if args[0] == "KEYSTONE.FEED.CLOSE":
            return SimpleNamespace(kind="simple", value=b"OK")
        return next(self.replies)


def stager(controller=None, **kwargs):
    return FeedStager(controller or FakeController([]), tenant_id=77,
                      clearance=2, sci=1, prefix="suite/", **kwargs)


class EnvelopeTests(unittest.TestCase):
    def test_all_16_fields_and_drained_sentinel(self):
        rec = parse_next(envelope(flags=TOMBSTONE | SECURITY_SENSITIVE))
        self.assertEqual((rec.classification, rec.sci, rec.tenant_id), (2, 1, 77))
        self.assertEqual((rec.object_type, rec.fencing_epoch, rec.offset), (42, 1, 1))
        self.assertTrue(rec.tombstone)
        self.assertIsNone(parse_next(item("int", 0)))

    def test_every_field_wrong_type_rejected(self):
        for position in range(16):
            with self.subTest(position=position):
                reply = envelope()
                reply.items[position] = item("nil", None)
                with self.assertRaises(FeedCorruption):
                    parse_next(reply)

    def test_corrupt_sizes_and_encoding_rejected(self):
        cases = [(2, item("bulk", b"\xff")),
                 (2, item("bulk", b"x" * 1025)),
                 (7, item("bulk", b"x" * (256 * 1024 + 1))),
                 (8, item("bulk", b"bad-uuid")),
                 (3, item("int", -1)),
                 (14, item("int", 1 << 15))]
        for position, corrupt in cases:
            with self.subTest(position=position):
                reply = envelope()
                reply.items[position] = corrupt
                with self.assertRaises(FeedCorruption):
                    parse_next(reply)
        for reply in (item("int", 1), SimpleNamespace(kind="array", items=[])):
            with self.assertRaises(FeedCorruption):
                parse_next(reply)


class StagingTests(unittest.TestCase):
    def test_replay_replacement_and_tombstone(self):
        stage = stager()
        first = parse_next(envelope())
        self.assertTrue(stage.apply(first))
        self.assertFalse(stage.apply(first))
        second = parse_next(envelope(event=2, generation=2, payload=b"new"))
        self.assertTrue(stage.apply(second))
        tomb = parse_next(envelope(event=3, generation=3,
                                   flags=TOMBSTONE, payload=b""))
        self.assertTrue(stage.apply(tomb))
        self.assertTrue(stage.objects[first.object_id].tombstone)
        self.assertFalse(stage.apply(first))
        with self.assertRaises(FeedCorruption):
            stage.apply(parse_next(envelope(event=4, generation=3)))
        resurrect = parse_next(envelope(event=4, generation=4, payload=b"again"))
        self.assertTrue(stage.apply(resurrect))
        self.assertFalse(stage.objects[first.object_id].tombstone)

    def test_old_epoch_duplicate_replay_is_idempotent(self):
        stage = stager()
        first = parse_next(envelope())
        stage.apply(first)
        later = replace(parse_next(envelope(event=2, generation=2)), fencing_epoch=2)
        stage.apply(later)
        self.assertFalse(stage.apply(first))
        with self.assertRaises(FeedCorruption):
            stage.apply(replace(parse_next(envelope(event=3, generation=3)),
                                fencing_epoch=1))

    def test_mixed_objects_do_not_share_fencing_epoch(self):
        stage = stager()
        first = replace(parse_next(envelope()), fencing_epoch=9)
        other = replace(parse_next(envelope(event=2)),
                        object_id=str(uuid.UUID(int=201)),
                        resource_id="suite/artifact/2", fencing_epoch=1)
        self.assertTrue(stage.apply(first))
        self.assertTrue(stage.apply(other))
        self.assertEqual(len(stage.objects), 2)
        with self.assertRaises(FeedCorruption):
            stage.apply(replace(parse_next(envelope(event=3, generation=2)),
                                fencing_epoch=8))

    def test_aggregate_byte_cap_replacement_and_tombstone_release(self):
        stage = stager(max_staged_bytes=8)
        first = parse_next(envelope(payload=b"abcdefgh"))
        stage.apply(first)
        self.assertEqual(stage.staged_bytes, 8)
        other = replace(parse_next(envelope(event=2, payload=b"x")),
                        object_id=str(uuid.UUID(int=201)),
                        resource_id="suite/artifact/2")
        with self.assertRaises(FeedCorruption):
            stage.apply(other)
        self.assertEqual(stage.staged_bytes, 8)
        self.assertNotIn(other.object_id, stage.objects)
        shorter = parse_next(envelope(event=3, generation=2, payload=b"abcd"))
        stage.apply(shorter)
        self.assertEqual(stage.staged_bytes, 4)
        stage.apply(other)
        self.assertEqual(stage.staged_bytes, 5)
        tomb = parse_next(envelope(event=4, generation=3, flags=TOMBSTONE,
                                   payload=b"do-not-retain"))
        stage.apply(tomb)
        self.assertEqual(stage.staged_bytes, 1)
        self.assertEqual(stage.objects[first.object_id].payload, b"")
        self.assertFalse(stage.apply(tomb))
        self.assertEqual(stage.staged_bytes, 1)

    def test_conflicts_security_and_bounds_fail_closed(self):
        stage = stager(max_objects=1)
        first = parse_next(envelope())
        stage.apply(first)
        for bad in (replace(first, payload=b"changed"),
                    replace(first, event_id=str(__import__("uuid").UUID(int=99))),
                    replace(first, tenant_id=78),
                    replace(first, classification=3),
                    replace(first, sci=2),
                    replace(first, resource_id="other/artifact"),
                    replace(first, fencing_epoch=0, event_id=str(__import__("uuid").UUID(int=98)))):
            with self.subTest(bad=bad):
                with self.assertRaises(FeedCorruption):
                    stage.apply(bad)
        self.assertEqual(stage.objects[first.object_id], first)

    def test_read_path_never_acks(self):
        ctrl = FakeController([envelope(), envelope(), item("int", 0)])
        stage = stager(ctrl)
        stage.open()
        self.assertIsNotNone(stage.next())
        self.assertIsNotNone(stage.next())  # duplicate delivery
        self.assertIsNone(stage.next())
        stage.close()
        self.assertEqual(len(stage.objects), 1)
        self.assertEqual([c[0] for c in ctrl.commands],
                         ["OPEN", "KEYSTONE.FEED.NEXT", "KEYSTONE.FEED.NEXT",
                          "KEYSTONE.FEED.NEXT", "KEYSTONE.FEED.CLOSE"])

    def test_corruption_stops_before_stage_mutation_or_ack(self):
        bad = envelope()
        bad.items[14] = item("nil", None)
        ctrl = FakeController([bad])
        stage = stager(ctrl)
        stage.open()
        with self.assertRaises(FeedCorruption):
            stage.next()
        self.assertFalse(stage.objects)
        self.assertTrue(stage.failed)
        with self.assertRaises(RuntimeError):
            stage.next()
        self.assertNotIn("KEYSTONE.FEED.ACK", [c[0] for c in ctrl.commands])

    def test_connection_requires_explicit_credentials_and_remote_qkp(self):
        with self.assertRaises(ValueError):
            FeedStager.connect("127.0.0.1", 6390, username="", password="",
                               tenant_id=77, clearance=2, sci=1, prefix="suite/")
        with self.assertRaises(ValueError):
            FeedStager.connect("example.invalid", 6390, username="idx",
                               password="secret", tenant_id=77, clearance=2,
                               sci=1, prefix="suite/")
        with self.assertRaises(ValueError):
            stager(max_staged_bytes=32 * 1024 * 1024 + 1)

    def test_installed_qihse_controller_connect_signature(self):
        # QIHSE's package initializer writes a native audit log in cwd on
        # import; isolate that unrelated side effect from the KEYSTONE tree.
        original_dir = os.getcwd()
        with tempfile.TemporaryDirectory() as scratch:
            try:
                os.chdir(scratch)
                try:
                    from qihse import controller as sdk
                except ImportError:
                    self.skipTest("QIHSE SDK not installed in this environment")
                with mock.patch.object(sdk, "Controller", autospec=True) as make:
                    stage = FeedStager.connect("127.0.0.1", 6390,
                                               username="indexer", password="example",
                                               tenant_id=77, clearance=2, sci=1,
                                               prefix="suite/", max_staged_bytes=1024)
                    self.assertIs(stage.controller, make.return_value)
                    self.assertEqual(stage.max_staged_bytes, 1024)
                    make.assert_called_once_with(
                        "127.0.0.1", 6390, username="indexer", password="example",
                        qkp_identity_dir=None, qkp_trusted_pubs=None,
                        qkp_node_id=None)
            finally:
                os.chdir(original_dir)


if __name__ == "__main__":
    unittest.main()
