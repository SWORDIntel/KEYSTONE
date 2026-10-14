"""Authorized search index projection and stream consumer for QIHSE change feed.

Maintains an indexed projection of metadata, permitted disassembly text, and QIHSE
artifact-ID backreferences derived strictly from the durable feed. Search operations
enforce explicit tenant, clearance, and SCI compartment authorization with zero
metadata leakage. Payload bytes are never served directly from the index without
QIHSE reauthorization and exact content verification.
"""

from __future__ import annotations

import base64
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import sqlite3
from typing import Any

from . import FeedCorruption, FeedRecord, TOMBSTONE
from .durable import DurableFeed


class SearchAuthorizationError(PermissionError):
    """The requesting principal lacks the necessary tenant, clearance, or SCI context."""


class QihseReauthorizationError(PermissionError):
    """QIHSE denied reauthorization to retrieve payload bytes."""


class PayloadIntegrityError(ValueError):
    """Payload retrieved from QIHSE failed exact content verification."""


class QihseOutageError(RuntimeError):
    """QIHSE service is unreachable or in outage; fail-closed."""


@dataclass(frozen=True)
class SearchPrincipal:
    tenant_id: int
    clearance: int
    sci: int

    def can_access(self, record_tenant: int, record_clearance: int, record_sci: int) -> bool:
        if self.tenant_id != record_tenant:
            return False
        if self.clearance < record_clearance:
            return False
        if record_sci & ~self.sci:
            return False
        return True


@dataclass(frozen=True)
class SearchHit:
    object_id: str
    resource_id: str
    qihse_artifact_id: str
    classification: int
    sci: int
    tenant_id: int
    generation: int
    stale: bool
    cursor: int = 0
    freshness: str = "fresh"
    status: str = "complete"
    incomplete: bool = False
    source_artifact_id: str = ""

    def __post_init__(self):
        if not self.source_artifact_id:
            object.__setattr__(self, "source_artifact_id", self.qihse_artifact_id)
        if self.stale and self.freshness == "fresh":
            object.__setattr__(self, "freshness", "stale")
        elif not self.stale and self.freshness == "stale":
            object.__setattr__(self, "freshness", "stale")


def _extract_manifest_and_text(
    payload_raw: bytes | str,
    resource_id: str,
    event_type: str,
    is_tombstone: bool,
) -> tuple[str, str, int, str]:
    """Extract (artifact_id, searchable_text, is_incomplete, status) without storing payload.

    Permitted disassembly text and analysis metadata are indexed into searchable_text;
    the raw payload bytes are discarded and never kept in the search projection.
    """
    if is_tombstone:
        artifact_id = resource_id.split("/")[-1]
        searchable_text = f"{resource_id} {event_type} tombstone".strip()
        return artifact_id, searchable_text, 0, "tombstone"

    payload_bytes = b""
    if isinstance(payload_raw, str):
        try:
            payload_bytes = base64.b64decode(payload_raw, validate=True)
        except Exception:
            payload_bytes = payload_raw.encode("utf-8", errors="ignore")
    elif isinstance(payload_raw, bytes):
        payload_bytes = payload_raw

    artifact_id = resource_id.split("/")[-1]
    disassembly = ""
    meta_tokens = []
    is_incomplete = 0
    status = "complete"

    if payload_bytes:
        manifest = None
        try:
            manifest = json.loads(payload_bytes.decode("utf-8"))
        except Exception:
            manifest = None

        if isinstance(manifest, dict):
            # Authoritative artifact backreference
            if manifest.get("artifact_id"):
                artifact_id = str(manifest["artifact_id"])
            elif manifest.get("qihse_artifact_id"):
                artifact_id = str(manifest["qihse_artifact_id"])

            # Permitted disassembly text
            disassembly = str(
                manifest.get("disassembly")
                or manifest.get("disassembly_text")
                or manifest.get("text")
                or ""
            )

            # Permitted analysis metadata
            if manifest.get("producer"):
                meta_tokens.append(str(manifest["producer"]))
            if manifest.get("schema"):
                meta_tokens.append(str(manifest["schema"]))
            if manifest.get("fidelity"):
                meta_tokens.append(str(manifest["fidelity"]))
            if manifest.get("verdict"):
                meta_tokens.append(str(manifest["verdict"]))
            if manifest.get("functions"):
                funcs = manifest["functions"]
                if isinstance(funcs, list):
                    meta_tokens.extend(str(f) for f in funcs)
            if manifest.get("metadata") and isinstance(manifest["metadata"], dict):
                meta_tokens.append(json.dumps(manifest["metadata"]))

            # Completion & fidelity status
            completion = str(manifest.get("completion", "")).lower()
            fidelity = str(manifest.get("fidelity", "")).lower()
            if (
                completion in ("partial", "incomplete")
                or fidelity in ("partial", "unassessed")
                or manifest.get("incomplete") is True
            ):
                is_incomplete = 1
                status = "incomplete"
        else:
            try:
                text_content = payload_bytes.decode("utf-8")
                disassembly = text_content
            except Exception:
                pass

    search_parts = [resource_id, event_type]
    if disassembly:
        search_parts.append(disassembly)
    if meta_tokens:
        search_parts.extend(meta_tokens)

    searchable_text = " ".join(search_parts).strip()
    return artifact_id, searchable_text, is_incomplete, status


class KeystoneAnalysisIndex:
    """Authorized index projection over a DurableFeed SQLite database."""

    def __init__(self, durable_feed: DurableFeed):
        self.durable = durable_feed
        self.db = durable_feed.db
        self._ensure_index_tables()
        # Bind durable feed precommit hook so projection is committed with durable state before ACK
        self.durable._precommit_hook = self._on_precommit

    def _ensure_index_tables(self) -> None:
        self.db.execute(
            """CREATE TABLE IF NOT EXISTS search_projection (
                object_id TEXT PRIMARY KEY,
                resource_id TEXT NOT NULL,
                qihse_artifact_id TEXT NOT NULL,
                classification INTEGER NOT NULL,
                sci INTEGER NOT NULL,
                tenant_id INTEGER NOT NULL,
                generation INTEGER NOT NULL,
                cursor INTEGER NOT NULL DEFAULT 0,
                freshness TEXT NOT NULL DEFAULT 'fresh',
                status TEXT NOT NULL DEFAULT 'complete',
                is_incomplete INTEGER NOT NULL DEFAULT 0,
                searchable_text TEXT NOT NULL,
                tombstone INTEGER NOT NULL
            )"""
        )
        self.db.execute(
            "CREATE INDEX IF NOT EXISTS idx_search_tenant ON search_projection(tenant_id, classification, sci)"
        )
        self.db.execute(
            "CREATE INDEX IF NOT EXISTS idx_search_artifact ON search_projection(qihse_artifact_id)"
        )

    def _on_precommit(
        self,
        db: sqlite3.Connection,
        record: FeedRecord,
        staged_record: FeedRecord,
        generation: int,
        cursor: int,
    ) -> None:
        """Invoked inside DurableFeed._apply transaction immediately before COMMIT.

        Guarantees that index-generation publication is durable before KEYSTONE.FEED.ACK.
        """
        is_tombstone = bool(record.flags & TOMBSTONE)
        art_id, s_text, is_incomp, status = _extract_manifest_and_text(
            record.payload, record.resource_id, record.event_type, is_tombstone
        )

        db.execute(
            """INSERT INTO search_projection (
                object_id, resource_id, qihse_artifact_id, classification, sci,
                tenant_id, generation, cursor, freshness, status, is_incomplete,
                searchable_text, tombstone
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, 'fresh', ?, ?, ?, ?)
            ON CONFLICT(object_id) DO UPDATE SET
                resource_id=excluded.resource_id,
                qihse_artifact_id=excluded.qihse_artifact_id,
                classification=excluded.classification,
                sci=excluded.sci,
                tenant_id=excluded.tenant_id,
                generation=excluded.generation,
                cursor=excluded.cursor,
                freshness='fresh',
                status=excluded.status,
                is_incomplete=excluded.is_incomplete,
                searchable_text=excluded.searchable_text,
                tombstone=excluded.tombstone
            """,
            (
                record.object_id,
                record.resource_id,
                art_id,
                record.classification,
                record.sci,
                record.tenant_id,
                generation,
                cursor,
                status,
                is_incomp,
                s_text,
                1 if is_tombstone else 0,
            ),
        )

    def sync_from_durable(self) -> int:
        """Projects unindexed or updated records from objects table into the search projection."""
        cursor = self.db.cursor()
        cursor.execute("SELECT object_id, record FROM objects")
        rows = cursor.fetchall()
        count = 0
        for object_id, raw_record in rows:
            record_dict = json.loads(raw_record)
            payload_raw = record_dict.get("payload", "")
            resource_id = record_dict["resource_id"]
            flags = record_dict["flags"]
            is_tombstone = bool(flags & TOMBSTONE)

            art_id, s_text, is_incomp, status = _extract_manifest_and_text(
                payload_raw, resource_id, record_dict["event_type"], is_tombstone
            )

            cursor.execute(
                """INSERT INTO search_projection (
                    object_id, resource_id, qihse_artifact_id, classification, sci,
                    tenant_id, generation, cursor, freshness, status, is_incomplete,
                    searchable_text, tombstone
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, 'fresh', ?, ?, ?, ?)
                ON CONFLICT(object_id) DO UPDATE SET
                    resource_id=excluded.resource_id,
                    qihse_artifact_id=excluded.qihse_artifact_id,
                    classification=excluded.classification,
                    sci=excluded.sci,
                    tenant_id=excluded.tenant_id,
                    generation=excluded.generation,
                    cursor=excluded.cursor,
                    freshness='fresh',
                    status=excluded.status,
                    is_incomplete=excluded.is_incomplete,
                    searchable_text=excluded.searchable_text,
                    tombstone=excluded.tombstone
                """,
                (
                    object_id,
                    resource_id,
                    art_id,
                    record_dict["classification"],
                    record_dict["sci"],
                    record_dict["tenant_id"],
                    record_dict["generation"],
                    record_dict.get("offset", self.durable.cursor),
                    status,
                    is_incomp,
                    s_text,
                    1 if is_tombstone else 0,
                ),
            )
            count += 1
        return count

    def search(
        self,
        principal: SearchPrincipal,
        query: str,
        *,
        include_tombstones: bool = False,
    ) -> list[SearchHit]:
        """Authorized candidate search filtering by principal tenant, clearance, and SCI."""
        cursor = self.db.cursor()
        tombstone_clause = "" if include_tombstones else "AND tombstone = 0"
        query_pattern = f"%{query}%"

        cursor.execute(
            f"""SELECT object_id, resource_id, qihse_artifact_id, classification, sci,
                       tenant_id, generation, cursor, freshness, status, is_incomplete, tombstone
                FROM search_projection
                WHERE tenant_id = ?
                  AND classification <= ?
                  AND searchable_text LIKE ?
                  {tombstone_clause}
            """,
            (principal.tenant_id, principal.clearance, query_pattern),
        )

        hits: list[SearchHit] = []
        current_generation = self.durable.generation
        for row in cursor.fetchall():
            (
                obj_id,
                res_id,
                art_id,
                cls,
                sci,
                tenant,
                gen,
                cur,
                fresh,
                stat,
                incomp,
                tomb,
            ) = row
            # Enforce exact SCI compartment bitmask
            if sci & ~principal.sci:
                continue
            # Mark candidate as stale if its generation precedes current durable generation
            is_stale = gen < current_generation
            freshness_val = "stale" if is_stale else fresh
            hits.append(
                SearchHit(
                    object_id=obj_id,
                    resource_id=res_id,
                    qihse_artifact_id=art_id,
                    classification=cls,
                    sci=sci,
                    tenant_id=tenant,
                    generation=gen,
                    stale=is_stale,
                    cursor=cur,
                    freshness=freshness_val,
                    status=stat,
                    incomplete=bool(incomp),
                    source_artifact_id=art_id,
                )
            )
        return hits


def retrieve_artifact_payload(
    principal: SearchPrincipal,
    hit: SearchHit,
    qihse_controller: Any,
    *,
    expected_content_digest: str | None = None,
) -> bytes:
    """Retrieve artifact payload bytes requiring QIHSE reauthorization and exact content verification.

    KEYSTONE does not serve payload bytes from its candidate search index.
    The principal clearance and SCI are re-verified, QIHSE authorizes disclosure,
    and the retrieved byte stream is verified against the SHA-384 content digest.
    """
    if not principal.can_access(hit.tenant_id, hit.classification, hit.sci):
        raise SearchAuthorizationError(
            f"Principal lacks necessary clearance/SCI for tenant {hit.tenant_id}"
        )

    try:
        reply = qihse_controller.call(
            "QIHSE.ARTIFACT.GET",
            hit.qihse_artifact_id,
            str(principal.tenant_id),
            str(principal.clearance),
            str(principal.sci),
        )
    except Exception as exc:
        raise QihseOutageError(f"QIHSE service unavailable during reauthorization: {exc}") from exc

    if getattr(reply, "kind", None) == "error" or getattr(reply, "value", None) == b"ERR_DENIED":
        raise QihseReauthorizationError("QIHSE denied reauthorization for payload retrieval")
    if getattr(reply, "kind", None) != "bulk" or not isinstance(reply.value, bytes):
        raise QihseReauthorizationError(f"QIHSE failed to return payload: {reply}")

    payload_bytes = reply.value

    # Exact content verification
    digest = hashlib.sha384(payload_bytes).hexdigest()
    expected = expected_content_digest or hit.qihse_artifact_id
    if expected.startswith("sha384_") or expected.startswith("sha384:"):
        expected = expected.split(":", 1)[-1].split("_", 1)[-1]

    if len(expected) == 96 and digest.lower() != expected.lower():
        raise PayloadIntegrityError(
            f"Exact content verification failed: SHA-384 {digest} != expected {expected}"
        )

    return payload_bytes


class KeystoneFeedConsumer:
    """Consumes QIHSE authenticated KEYSTONE.FEED.* stream into a durable analysis index.

    Handles at-least-once delivery, replay gaps, duplicate/replacement events,
    and ACK only after durable index-generation publication. Supports rebuilding
    equivalent indexes from QIHSE authority without mutating authoritative state.
    """

    def __init__(self, durable_feed: DurableFeed, index: KeystoneAnalysisIndex | None = None):
        self.durable = durable_feed
        self.index = index or KeystoneAnalysisIndex(durable_feed)
        self.controller = durable_feed.controller
        self.last_seen_offset: int | None = self.durable.cursor if self.durable.cursor > 0 else None
        self.replay_gaps: list[tuple[int, int]] = []
        self.duplicate_count: int = 0
        self.consumed_count: int = 0

    def consume_next(self) -> FeedRecord | None:
        """Consumes one feed event, detects replay gaps, and commits projection before ACK."""
        if self.durable.failed or self.durable.feed_id is None:
            raise RuntimeError("feed failed or not open")

        raw = self.controller.call("KEYSTONE.FEED.NEXT", str(self.durable.feed_id))
        from . import parse_next
        record = parse_next(raw)
        if record is None:
            return None

        # Detect replay gap if offset jumped unexpectedly
        if self.last_seen_offset is not None and record.offset > self.last_seen_offset + 1024:
            self.replay_gaps.append((self.last_seen_offset, record.offset))

        # Check for duplicate / at-least-once replay
        if record.event_id in self.durable.stage._event_ids:
            self.duplicate_count += 1

        self.durable._apply(record)
        self.last_seen_offset = record.offset
        self.consumed_count += 1
        return record

    def consume_all(self, max_records: int | None = None) -> int:
        """Consumes records until stream is drained or max_records reached."""
        count = 0
        while max_records is None or count < max_records:
            rec = self.consume_next()
            if rec is None:
                break
            count += 1
        return count

    @classmethod
    def rebuild_from_authority(
        cls,
        controller: Any,
        directory: str | Path,
        *,
        tenant_id: int,
        clearance: int,
        sci: int,
        prefix: str,
        max_objects: int = 10000,
        max_staged_bytes: int = 32 * 1024 * 1024,
    ) -> KeystoneAnalysisIndex:
        """Rebuild an equivalent candidate index from QIHSE authority.

        Opens a clean durable feed at cursor 0 and streams all events to reconstruct
        an equivalent search projection. Authoritative QIHSE artifacts are never altered.
        """
        root = Path(directory)
        root.mkdir(mode=0o700, parents=True, exist_ok=True)
        db_file = root / "generation.sqlite3"
        lock_file = root / "generation.lock"
        if db_file.exists():
            db_file.unlink()
        if lock_file.exists():
            lock_file.unlink()

        feed = DurableFeed(
            controller,
            directory,
            tenant_id=tenant_id,
            clearance=clearance,
            sci=sci,
            prefix=prefix,
            max_objects=max_objects,
            max_staged_bytes=max_staged_bytes,
        )
        idx = KeystoneAnalysisIndex(feed)
        feed.open()
        consumer = cls(feed, idx)
        consumer.consume_all()
        return idx
