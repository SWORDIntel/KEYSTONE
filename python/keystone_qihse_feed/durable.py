"""Bounded, non-serving durable QIHSE feed generation and cursor.

The SQLite transaction is the publication boundary: object state, replay IDs,
generation, and source offset commit together. No index/query API is exposed.
"""

from __future__ import annotations

from dataclasses import asdict
import base64
import fcntl
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import stat

from . import FeedCorruption, FeedRecord, FeedStager, parse_next


class DurableCorruption(FeedCorruption):
    """The persisted generation is damaged or belongs to another scope."""


def _encode(record: FeedRecord) -> str:
    value = asdict(record)
    value["payload"] = base64.b64encode(record.payload).decode("ascii")
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def _decode(raw: str) -> FeedRecord:
    value = json.loads(raw)
    if set(value) != set(FeedRecord.__dataclass_fields__):
        raise DurableCorruption("stored record fields differ from schema")
    payload = value["payload"]
    if type(payload) is not str:
        raise DurableCorruption("stored payload is not base64")
    value["payload"] = base64.b64decode(payload, validate=True)
    record = FeedRecord(**value)
    if _encode(record) != raw:
        raise DurableCorruption("noncanonical stored record")
    return record


def _digest(db: sqlite3.Connection, scope: str, generation: int, cursor: int) -> str:
    h = hashlib.sha384()
    for field in ("qihse-feed-generation-v1", scope, str(generation), str(cursor)):
        encoded = field.encode("utf-8")
        h.update(len(encoded).to_bytes(8, "big"))
        h.update(encoded)
    for table, columns in (("events", "event_id, signature, offset"),
                           ("objects", "object_id, record")):
        h.update(table.encode("ascii"))
        for row in db.execute(f"SELECT {columns} FROM {table} ORDER BY 1"):
            for field in row:
                data = field if type(field) is bytes else str(field).encode("utf-8")
                h.update(len(data).to_bytes(8, "big"))
                h.update(data)
    return h.hexdigest()


class DurableFeed:
    """One writer, bounded durable staging; never serves analyst queries.

    `directory` must be a dedicated local filesystem directory. The caller
    supplies an already authenticated QIHSE Controller. No credentials or
    trust roots are persisted. A process-wide lock prevents two writers.
    """

    def __init__(self, controller, directory: str | Path, *, tenant_id: int,
                 clearance: int, sci: int, prefix: str, max_objects: int = 10000,
                 max_staged_bytes: int = 32 * 1024 * 1024):
        self.stage = FeedStager(controller, tenant_id=tenant_id,
                                clearance=clearance, sci=sci, prefix=prefix,
                                max_objects=max_objects,
                                max_staged_bytes=max_staged_bytes)
        self.controller = controller
        self.scope = json.dumps([tenant_id, clearance, sci, prefix, max_objects,
                                 max_staged_bytes], separators=(",", ":"))
        root = Path(directory)
        if root.is_symlink():
            raise ValueError("feed directory cannot be a symlink")
        root.mkdir(mode=0o700, parents=True, exist_ok=True)
        if stat.S_IMODE(root.stat().st_mode) & 0o077:
            raise ValueError("feed directory must not be accessible by other users")
        self._lock_fd = os.open(root / "generation.lock", os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
        try:
            fcntl.flock(self._lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            path = root / "generation.sqlite3"
            if path.is_symlink():
                raise DurableCorruption("generation database cannot be a symlink")
            if not path.exists():
                os.close(os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR | os.O_NOFOLLOW, 0o600))
            self.db = sqlite3.connect(path, isolation_level=None)
            if not path.exists():
                raise DurableCorruption("database disappeared during open")
            os.chmod(path, 0o600)
            self.db.execute("PRAGMA journal_mode=DELETE")
            self.db.execute("PRAGMA synchronous=FULL")
            self.db.execute("PRAGMA trusted_schema=OFF")
            self.db.execute("PRAGMA foreign_keys=ON")
            self._initialize_or_load()
            dir_fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(dir_fd)
            finally:
                os.close(dir_fd)
        except Exception:
            if hasattr(self, "db"):
                self.db.close()
            os.close(self._lock_fd)
            raise
        self.feed_id: int | None = None
        self.failed = False
        self._precommit_hook = None

    def _initialize_or_load(self) -> None:
        db = self.db
        if db.execute("PRAGMA quick_check").fetchone() != ("ok",):
            raise DurableCorruption("SQLite integrity check failed")
        db.execute("CREATE TABLE IF NOT EXISTS meta (singleton INTEGER PRIMARY KEY CHECK(singleton=1), version INTEGER NOT NULL, scope TEXT NOT NULL, generation INTEGER NOT NULL, cursor TEXT NOT NULL, digest TEXT NOT NULL)")
        db.execute("CREATE TABLE IF NOT EXISTS events (event_id TEXT PRIMARY KEY, signature BLOB NOT NULL, offset TEXT NOT NULL UNIQUE)")
        db.execute("CREATE TABLE IF NOT EXISTS objects (object_id TEXT PRIMARY KEY, record TEXT NOT NULL)")
        meta = db.execute("SELECT version, scope, generation, cursor, digest FROM meta WHERE singleton=1").fetchone()
        if meta is None:
            # A missing meta row is only legitimate in a completely new DB.
            if db.execute("SELECT COUNT(*) FROM events").fetchone()[0] or db.execute("SELECT COUNT(*) FROM objects").fetchone()[0]:
                raise DurableCorruption("state exists without generation metadata")
            self.generation, self.cursor = 0, 0
            digest = _digest(db, self.scope, 0, 0)
            db.execute("INSERT INTO meta VALUES (1, 1, ?, 0, '0', ?)", (self.scope, digest))
            return
        version, scope, generation, cursor_text, digest = meta
        if version != 1 or scope != self.scope or type(generation) is not int or generation < 0:
            raise DurableCorruption("generation schema or security scope mismatch")
        if not cursor_text.isdecimal() or str(int(cursor_text)) != cursor_text:
            raise DurableCorruption("invalid durable cursor")
        self.generation, self.cursor = generation, int(cursor_text)
        if self.cursor >= 1 << 64 or digest != _digest(db, scope, generation, self.cursor):
            raise DurableCorruption("generation digest mismatch")
        events = db.execute("SELECT event_id, signature, offset FROM events").fetchall()
        if len(events) > self.stage.max_objects * 4:
            raise DurableCorruption("stored event bound exceeded")
        self.stage._event_ids = {}
        for event_id, signature, offset in events:
            if type(signature) is not bytes or len(signature) != 32 or not offset.isdecimal() or int(offset) > self.cursor:
                raise DurableCorruption("invalid stored replay record")
            self.stage._event_ids[event_id] = signature
        objects = db.execute("SELECT object_id, record FROM objects").fetchall()
        if len(objects) > self.stage.max_objects:
            raise DurableCorruption("stored object bound exceeded")
        for object_id, raw in objects:
            record = _decode(raw)
            if record.object_id != object_id or record.tenant_id != self.stage.tenant_id or record.classification > self.stage.clearance or record.sci & ~self.stage.sci or not record.resource_id.startswith(self.stage.prefix):
                raise DurableCorruption("stored object violates security scope")
            self.stage.objects[object_id] = record
            self.stage.staged_bytes += len(record.payload)
        if self.stage.staged_bytes > self.stage.max_staged_bytes:
            raise DurableCorruption("stored payload bound exceeded")

    def open(self) -> None:
        if self.failed or self.feed_id is not None:
            raise RuntimeError("feed failed or already open")
        feed = self.controller.keystone_feed_open(prefix=self.stage.prefix, cursor=self.cursor)
        self.feed_id = feed.feed_id

    def next(self) -> FeedRecord | None:
        if self.failed or self.feed_id is None:
            raise RuntimeError("feed failed or not open")
        try:
            record = parse_next(self.controller.call("KEYSTONE.FEED.NEXT", str(self.feed_id)))
            if record is None:
                return None
            self._apply(record)
            return record
        except Exception:
            self.failed = True
            raise

    def _apply(self, record: FeedRecord) -> bool:
        if self.failed or self.feed_id is None:
            raise RuntimeError("feed failed or not open")
        # Stage in a copy. A failed SQLite transaction must never advance
        # in-memory state, and a restart can replay from the previous cursor.
        proposed = FeedStager(self.controller, tenant_id=self.stage.tenant_id,
                              clearance=self.stage.clearance, sci=self.stage.sci,
                              prefix=self.stage.prefix, max_objects=self.stage.max_objects,
                              max_staged_bytes=self.stage.max_staged_bytes)
        proposed.objects = self.stage.objects.copy()
        proposed._event_ids = self.stage._event_ids.copy()
        proposed.staged_bytes = self.stage.staged_bytes
        changed = proposed.apply(record)
        if not changed:
            if record.offset > self.cursor:
                raise FeedCorruption("duplicate event beyond durable cursor")
            return False
        if (record.offset <= self.cursor and
                not (self.generation == 0 and record.offset == 0)) or record.offset >= 1 << 64:
            raise FeedCorruption("new event does not advance durable cursor")
        if self.generation >= (1 << 63) - 1:
            raise FeedCorruption("local generation exhausted")
        db = self.db
        try:
            db.execute("BEGIN IMMEDIATE")
            signature = proposed._event_ids[record.event_id]
            db.execute("INSERT INTO events VALUES (?, ?, ?)",
                       (record.event_id, signature, str(record.offset)))
            db.execute("INSERT INTO objects VALUES (?, ?) ON CONFLICT(object_id) DO UPDATE SET record=excluded.record",
                       (record.object_id, _encode(proposed.objects[record.object_id])))
            next_generation = self.generation + 1
            digest = _digest(db, self.scope, next_generation, record.offset)
            db.execute("UPDATE meta SET generation=?, cursor=?, digest=? WHERE singleton=1",
                       (next_generation, str(record.offset), digest))
            if self._precommit_hook:
                self._precommit_hook(db, record, proposed.objects[record.object_id], next_generation, record.offset)
            db.execute("COMMIT")
        except Exception:
            if db.in_transaction:
                db.execute("ROLLBACK")
            self.failed = True
            raise
        self.stage = proposed
        self.cursor = record.offset
        self.generation = next_generation
        # ACK is intentionally after COMMIT. Failure leaves durable state and
        # forbids continuation; a new process resumes from the stored offset.
        try:
            reply = self.controller.call("KEYSTONE.FEED.ACK", str(self.feed_id), str(self.cursor))
            if reply.kind != "simple" or reply.value != b"OK":
                raise FeedCorruption("ACK was not confirmed")
        except Exception:
            self.failed = True
            raise
        return True

    def close(self) -> None:
        try:
            if self.feed_id is not None:
                self.controller.call("KEYSTONE.FEED.CLOSE", str(self.feed_id))
                self.feed_id = None
        finally:
            self.db.close()
            os.close(self._lock_fd)
