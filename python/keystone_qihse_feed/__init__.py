"""Strict, non-serving QIHSE feed staging boundary.

The QIHSE Controller supplies explicit AUTH and bounded RESP framing. This
module validates the *raw* 16-field reply before any field coercion, stages
replay-safe object state, and intentionally never ACKs: no durable KEYSTONE
generation/cursor transaction exists yet. It is not a search endpoint.
"""

from __future__ import annotations

from dataclasses import dataclass, replace
import hashlib
import uuid


MAX_PAYLOAD = 256 * 1024  # QIHSE_KEYSTONE_FEED_MAX_PAYLOAD
MAX_TEXT = 1024
MAX_OBJECTS = 10000
MAX_STAGED_BYTES = 32 * 1024 * 1024
TOMBSTONE = 1
SECURITY_SENSITIVE = 1 << 5
KNOWN_FLAGS = (1 << 7) - 1


class FeedCorruption(ValueError):
    """A QIHSE feed reply violated its declared contract; stop ingestion."""


def _integer(item, bits: int, field: str) -> int:
    if item.kind != "int" or type(item.value) is not int:
        raise FeedCorruption(f"{field}: expected RESP integer")
    if not 0 <= item.value < (1 << bits):
        raise FeedCorruption(f"{field}: out of range")
    return item.value


def _bytes(item, field: str, limit: int) -> bytes:
    if item.kind != "bulk" or type(item.value) is not bytes:
        raise FeedCorruption(f"{field}: expected RESP bulk")
    if len(item.value) > limit:
        raise FeedCorruption(f"{field}: too large")
    return item.value


def _text(item, field: str, limit: int) -> str:
    raw = _bytes(item, field, limit)
    try:
        value = raw.decode("utf-8", "strict")
    except UnicodeDecodeError as exc:
        raise FeedCorruption(f"{field}: invalid UTF-8") from exc
    if not value or "\x00" in value:
        raise FeedCorruption(f"{field}: empty or NUL")
    return value


def _uuid(item, field: str) -> str:
    value = _text(item, field, 36)
    try:
        parsed = uuid.UUID(value)
    except ValueError as exc:
        raise FeedCorruption(f"{field}: invalid UUID") from exc
    if str(parsed) != value:
        raise FeedCorruption(f"{field}: noncanonical UUID")
    return value


@dataclass(frozen=True)
class FeedRecord:
    offset: int
    event_type: str
    resource_id: str
    classification: int
    sci: int
    tenant_id: int
    generation: int
    payload: bytes
    event_id: str
    origin_node: str
    object_id: str
    fencing_epoch: int
    hlc_physical_ms: int
    hlc_logical: int
    flags: int
    object_type: int

    @property
    def tombstone(self) -> bool:
        return bool(self.flags & TOMBSTONE)


def parse_next(reply) -> FeedRecord | None:
    """Parse one raw Controller.call('KEYSTONE.FEED.NEXT', ...) reply.

    An integer zero is the sole drained sentinel. Every other shape fails
    closed, including SDK FeedEvent's permissive wrong-type coercions.
    """
    if reply.kind == "int" and type(reply.value) is int and reply.value == 0:
        return None
    if reply.kind != "array" or len(reply.items) != 16:
        raise FeedCorruption("NEXT: expected exactly 16 items")
    p = reply.items
    record = FeedRecord(
        _integer(p[0], 64, "offset"),
        _text(p[1], "event_type", MAX_TEXT),
        _text(p[2], "resource_id", MAX_TEXT),
        _integer(p[3], 16, "classification"),
        _integer(p[4], 16, "sci"),
        _integer(p[5], 32, "tenant_id"),
        _integer(p[6], 64, "generation"),
        _bytes(p[7], "payload", MAX_PAYLOAD),
        _uuid(p[8], "event_id"),
        _uuid(p[9], "origin_node"),
        _uuid(p[10], "object_id"),
        _integer(p[11], 64, "fencing_epoch"),
        _integer(p[12], 64, "hlc_physical_ms"),
        _integer(p[13], 32, "hlc_logical"),
        _integer(p[14], 16, "flags"),
        _integer(p[15], 32, "object_type"),
    )
    if record.tenant_id == 0 or record.generation == 0:
        raise FeedCorruption("system tenant or zero generation")
    if record.flags & ~KNOWN_FLAGS:
        raise FeedCorruption("unknown flags")
    return record


class FeedStager:
    """Bounded in-memory ingestion, with no durable cursor and no serving API.

    The caller supplies an authenticated QIHSE Controller. Open permissions
    remain server-enforced. Expected tenant/clearance/SCI are an additional
    local fail-closed check, never a replacement for QIHSE authorization.
    """

    def __init__(self, controller, *, tenant_id: int, clearance: int,
                 sci: int, prefix: str, max_objects: int = MAX_OBJECTS,
                 max_staged_bytes: int = MAX_STAGED_BYTES):
        self._validate_scope(tenant_id, clearance, sci, prefix, max_objects,
                             max_staged_bytes)
        self.controller = controller
        self.tenant_id = tenant_id
        self.clearance = clearance
        self.sci = sci
        self.prefix = prefix
        self.max_objects = max_objects
        self.max_staged_bytes = max_staged_bytes
        self.staged_bytes = 0
        self.objects: dict[str, FeedRecord] = {}
        self._event_ids: dict[str, bytes] = {}
        self.feed_id: int | None = None
        self.failed = False

    @staticmethod
    def _validate_scope(tenant_id: int, clearance: int, sci: int,
                        prefix: str, max_objects: int,
                        max_staged_bytes: int) -> None:
        if not 0 < tenant_id < (1 << 32) or not 0 <= clearance < (1 << 16):
            raise ValueError("invalid tenant or clearance")
        if not 0 <= sci < (1 << 16) or not prefix or len(prefix.encode()) > 63:
            raise ValueError("invalid SCI or prefix")
        if not 0 < max_objects <= MAX_OBJECTS:
            raise ValueError("invalid object bound")
        if not 0 < max_staged_bytes <= MAX_STAGED_BYTES:
            raise ValueError("invalid staged-byte bound")

    @classmethod
    def connect(cls, host: str, port: int, *, username: str, password: str,
                tenant_id: int, clearance: int, sci: int, prefix: str,
                max_objects: int = MAX_OBJECTS,
                max_staged_bytes: int = MAX_STAGED_BYTES,
                qkp_identity_dir: str | None = None,
                qkp_trusted_pubs=None, qkp_node_id: str | None = None):
        """Use explicit credentials; secure remote transport needs QKP config.

        Plain RESP is limited to loopback. No credentials or trust roots are
        read from environment or embedded in this library.
        """
        if not username or not password:
            raise ValueError("explicit feed credentials required")
        if host not in ("127.0.0.1", "::1", "localhost") and not (
                qkp_identity_dir and qkp_trusted_pubs and qkp_node_id):
            raise ValueError("remote feed requires QKP identity and trust config")
        cls._validate_scope(tenant_id, clearance, sci, prefix, max_objects,
                            max_staged_bytes)
        from qihse.controller import Controller
        ctrl = Controller(host, port, username=username, password=password,
                          qkp_identity_dir=qkp_identity_dir,
                          qkp_trusted_pubs=qkp_trusted_pubs,
                          qkp_node_id=qkp_node_id)
        return cls(ctrl, tenant_id=tenant_id, clearance=clearance, sci=sci,
                   prefix=prefix, max_objects=max_objects,
                   max_staged_bytes=max_staged_bytes)

    def open(self) -> None:
        if self.failed:
            raise RuntimeError("failed feed stager cannot be reused")
        if self.feed_id is not None:
            raise RuntimeError("feed already open")
        if self.objects:
            raise RuntimeError("in-memory state cannot resume after feed closure")
        feed = self.controller.keystone_feed_open(prefix=self.prefix, cursor=0)
        self.feed_id = feed.feed_id

    def close(self) -> None:
        if self.feed_id is not None:
            self.controller.call("KEYSTONE.FEED.CLOSE", str(self.feed_id))
            self.feed_id = None

    def next(self) -> FeedRecord | None:
        if self.failed:
            raise RuntimeError("failed feed stager cannot continue")
        if self.feed_id is None:
            raise RuntimeError("feed not open")
        try:
            raw = self.controller.call("KEYSTONE.FEED.NEXT", str(self.feed_id))
            record = parse_next(raw)
            if record is not None:
                self.apply(record)
            return record
        except Exception:
            self.failed = True
            raise

    def apply(self, record: FeedRecord) -> bool:
        if (record.tenant_id != self.tenant_id or
                record.classification > self.clearance or
                record.sci & ~self.sci or
                not record.resource_id.startswith(self.prefix)):
            raise FeedCorruption("feed delivered record outside local security scope")
        previous = self.objects.get(record.object_id)
        signature = hashlib.sha256(repr(record).encode("utf-8")).digest()
        if record.event_id in self._event_ids:
            if self._event_ids[record.event_id] != signature:
                raise FeedCorruption("same event id has conflicting content")
            return False
        if len(self._event_ids) >= self.max_objects * 4:
            raise FeedCorruption("staging event limit reached")
        if previous is not None:
            if previous.resource_id != record.resource_id:
                raise FeedCorruption("object id changed resource identity")
            if record.fencing_epoch < previous.fencing_epoch:
                raise FeedCorruption("stale per-object fencing epoch")
            if previous.tombstone and not record.tombstone and (
                    record.generation <= previous.generation or
                    (record.hlc_physical_ms, record.hlc_logical) <=
                    (previous.hlc_physical_ms, previous.hlc_logical)):
                raise FeedCorruption("resurrection requires newer generation and HLC")
            if (record.generation, record.hlc_physical_ms, record.hlc_logical) <= (
                    previous.generation, previous.hlc_physical_ms,
                    previous.hlc_logical):
                raise FeedCorruption("stale or conflicting object replacement")
        elif len(self.objects) >= self.max_objects:
            raise FeedCorruption("staging object limit reached")
        # A tombstone's payload is not needed to represent deletion. Retain
        # the complete event signature for replay detection but release any
        # previously staged body (and do not retain the tombstone body).
        staged = replace(record, payload=b"") if record.tombstone else record
        new_bytes = self.staged_bytes - (len(previous.payload) if previous else 0) + len(staged.payload)
        if new_bytes > self.max_staged_bytes:
            raise FeedCorruption("staging byte limit reached")
        self.objects[record.object_id] = staged
        self.staged_bytes = new_bytes
        self._event_ids[record.event_id] = signature
        return True
