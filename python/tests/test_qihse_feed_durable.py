"""Transactional non-serving feed publication regression tests."""

from dataclasses import replace
from pathlib import Path
import os
import sqlite3
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from keystone_qihse_feed import FeedCorruption, parse_next, TOMBSTONE  # noqa: E402
from keystone_qihse_feed.durable import DurableCorruption, DurableFeed  # noqa: E402


def item(kind, value):
    return SimpleNamespace(kind=kind, value=value)


def envelope(offset=0, *, event=1, generation=1, flags=0, payload=b"manifest",
             clearance=2, sci=1):
    ident = lambda n: str(uuid.UUID(int=n)).encode("ascii")
    return SimpleNamespace(kind="array", items=[
        item("int", offset), item("bulk", b"suite.manifest"),
        item("bulk", b"suite/artifact/1"), item("int", clearance),
        item("int", sci), item("int", 77), item("int", generation),
        item("bulk", payload), item("bulk", ident(event)),
        item("bulk", ident(100)), item("bulk", ident(200)),
        item("int", 1), item("int", 1000 + event), item("int", 0),
        item("int", flags), item("int", 42)])


class Controller:
    def __init__(self, replies=(), *, fail_ack=False):
        self.replies = iter(replies)
        self.calls = []
        self.fail_ack = fail_ack

    def keystone_feed_open(self, **kwargs):
        self.calls.append(("OPEN", kwargs))
        return SimpleNamespace(feed_id=9)

    def call(self, *args):
        self.calls.append(args)
        if args[0] == "KEYSTONE.FEED.NEXT":
            return next(self.replies)
        if args[0] == "KEYSTONE.FEED.ACK" and self.fail_ack:
            raise OSError("connection dropped after commit")
        return SimpleNamespace(kind="simple", value=b"OK")


def feed(controller, directory, **kwargs):
    return DurableFeed(controller, directory, tenant_id=77, clearance=2,
                       sci=1, prefix="suite/", **kwargs)


class DurableFeedTests(unittest.TestCase):
    def test_commit_ack_resume_and_tombstone(self):
        with tempfile.TemporaryDirectory() as scratch:
            ctrl = Controller([envelope(), envelope(120, event=2, generation=2,
                                                     flags=TOMBSTONE, payload=b"discard"),
                               item("int", 0)])
            d = feed(ctrl, Path(scratch) / "feed")
            d.open()
            self.assertEqual(d.next().offset, 0)
            self.assertEqual(d.next().offset, 120)
            self.assertIsNone(d.next())
            self.assertEqual((d.generation, d.cursor, d.stage.staged_bytes), (2, 120, 0))
            d.close()
            self.assertEqual([c[0] for c in ctrl.calls],
                             ["OPEN", "KEYSTONE.FEED.NEXT", "KEYSTONE.FEED.ACK",
                              "KEYSTONE.FEED.NEXT", "KEYSTONE.FEED.ACK",
                              "KEYSTONE.FEED.NEXT", "KEYSTONE.FEED.CLOSE"])
            self.assertEqual([c[2] for c in ctrl.calls if c[0] == "KEYSTONE.FEED.ACK"],
                             ["0", "120"])
            replay = Controller([envelope(120, event=2, generation=2,
                                          flags=TOMBSTONE, payload=b"discard"), item("int", 0)])
            d2 = feed(replay, Path(scratch) / "feed")
            d2.open()
            self.assertEqual(replay.calls[0][1]["cursor"], 120)
            self.assertEqual(d2.next().offset, 120)
            self.assertIsNone(d2.next())
            self.assertEqual(d2.generation, 2)
            self.assertTrue(d2.stage.objects[str(uuid.UUID(int=200))].tombstone)
            self.assertNotIn("KEYSTONE.FEED.ACK", [call[0] for call in replay.calls])
            d2.close()

    def test_precommit_failure_rolls_back_without_ack(self):
        with tempfile.TemporaryDirectory() as scratch:
            ctrl = Controller([envelope()])
            d = feed(ctrl, Path(scratch) / "feed")
            d.open()
            d.db.execute("CREATE TRIGGER deny BEFORE INSERT ON events BEGIN SELECT RAISE(ABORT, 'injected'); END")
            with self.assertRaises(sqlite3.DatabaseError):
                d.next()
            self.assertEqual((d.generation, d.cursor, d.stage.objects), (0, 0, {}))
            self.assertNotIn("KEYSTONE.FEED.ACK", [c[0] for c in ctrl.calls])
            d.db.execute("DROP TRIGGER deny")
            d.close()
            d2 = feed(Controller(), Path(scratch) / "feed")
            self.assertEqual((d2.generation, d2.cursor, d2.stage.objects), (0, 0, {}))
            d2.close()

    def test_postcommit_ack_failure_restarts_from_durable_state(self):
        with tempfile.TemporaryDirectory() as scratch:
            ctrl = Controller([envelope()], fail_ack=True)
            d = feed(ctrl, Path(scratch) / "feed")
            d.open()
            with self.assertRaises(OSError):
                d.next()
            self.assertTrue(d.failed)
            self.assertEqual(d.generation, 1)
            d.close()
            next_ctrl = Controller([envelope(), envelope(200, event=2, generation=2)])
            d2 = feed(next_ctrl, Path(scratch) / "feed")
            d2.open()
            self.assertEqual(d2.next().offset, 0)
            self.assertEqual(d2.generation, 1)
            self.assertEqual(d2.next().offset, 200)
            self.assertEqual(d2.generation, 2)
            d2.close()

    def test_process_exit_after_commit_before_ack_recovers_generation(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch) / "feed"
            package_root = str(Path(__file__).resolve().parents[1])
            script = """
import os
from types import SimpleNamespace
from keystone_qihse_feed.durable import DurableFeed
from test_qihse_feed_durable import envelope
class CrashController:
    def keystone_feed_open(self, **kwargs):
        return SimpleNamespace(feed_id=9)
    def call(self, *args):
        if args[0] == 'KEYSTONE.FEED.NEXT':
            return envelope()
        if args[0] == 'KEYSTONE.FEED.ACK':
            os._exit(89)
        return SimpleNamespace(kind='simple', value=b'OK')
d = DurableFeed(CrashController(), os.environ['FEED_TEST_ROOT'],
                tenant_id=77, clearance=2, sci=1, prefix='suite/')
d.open()
d.next()
"""
            env = os.environ.copy()
            env["PYTHONPATH"] = os.pathsep.join((package_root,
                                                str(Path(__file__).resolve().parent)))
            env["FEED_TEST_ROOT"] = str(root)
            result = subprocess.run([sys.executable, "-c", script], env=env,
                                    capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 89, result.stderr)
            ctrl = Controller([envelope()])
            d = feed(ctrl, root)
            self.assertEqual((d.generation, d.cursor, len(d.stage.objects)), (1, 0, 1))
            d.open()
            d.next()
            self.assertEqual(d.generation, 1)
            self.assertNotIn("KEYSTONE.FEED.ACK", [c[0] for c in ctrl.calls])
            d.close()

    def test_scope_denial_and_tamper_fail_closed(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch) / "feed"
            for denied in (envelope(clearance=3), envelope(sci=2)):
                ctrl = Controller([denied])
                d = feed(ctrl, root)
                d.open()
                with self.assertRaises(FeedCorruption):
                    d.next()
                self.assertEqual((d.generation, d.cursor), (0, 0))
                self.assertNotIn("KEYSTONE.FEED.ACK", [c[0] for c in ctrl.calls])
                d.close()
            d = feed(Controller([envelope()]), root)
            d.open()
            d.next()
            d.close()
            with self.assertRaises(DurableCorruption):
                DurableFeed(Controller(), root, tenant_id=77, clearance=2,
                            sci=0, prefix="suite/")
            db = sqlite3.connect(root / "generation.sqlite3")
            db.execute("UPDATE objects SET record='{}'")
            db.commit()
            db.close()
            with self.assertRaises(DurableCorruption):
                feed(Controller(), root)

    def test_conflicting_replay_no_ack_and_one_writer(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch) / "feed"
            ctrl = Controller([envelope(), envelope(0, payload=b"altered")])
            d = feed(ctrl, root)
            with self.assertRaises(BlockingIOError):
                feed(Controller(), root)
            d.open()
            d.next()
            with self.assertRaises(FeedCorruption):
                d.next()
            self.assertEqual(d.generation, 1)
            self.assertEqual(len([c for c in ctrl.calls if c[0] == "KEYSTONE.FEED.ACK"]), 1)
            d.close()


if __name__ == "__main__":
    unittest.main()
