"""Authorized search index projection of the durable QIHSE change feed.

Maintains an indexed projection of metadata and QIHSE artifact-ID backreferences
derived strictly from the durable feed. Search operations enforce explicit
tenant, clearance, and SCI compartment authorization. Payload bytes are never
served directly from the index without reauthorization and verification.
"""

from __future__ import annotations

from dataclasses import dataclass
import json
import sqlite3
from typing import Any

from . import FeedCorruption, FeedRecord, TOMBSTONE
from .durable import DurableFeed


class SearchAuthorizationError(PermissionError):
    """The requesting principal lacks the necessary tenant, clearance, or SCI context."""


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


class KeystoneAnalysisIndex:
    """Authorized index projection over a DurableFeed SQLite database."""

    def __init__(self, durable_feed: DurableFeed):
        self.durable = durable_feed
        self.db = durable_feed.db
        self._ensure_index_tables()

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

    def sync_from_durable(self) -> int:
        """Projects unindexed or updated records from objects table into the search projection."""
        cursor = self.db.cursor()
        cursor.execute("SELECT object_id, record FROM objects")
        rows = cursor.fetchall()
        count = 0
        for object_id, raw_record in rows:
            record_dict = json.loads(raw_record)
            # Extract qihse_artifact_id backreference if carried in payload or resource_id
            payload_bytes = record_dict.get("payload", "")
            resource_id = record_dict["resource_id"]
            flags = record_dict["flags"]
            is_tombstone = 1 if (flags & TOMBSTONE) else 0

            # Derive backreference and searchable text
            artifact_backreference = resource_id.split("/")[-1]
            searchable_text = f"{resource_id} {record_dict['event_type']}"

            cursor.execute(
                """INSERT INTO search_projection (
                    object_id, resource_id, qihse_artifact_id, classification, sci,
                    tenant_id, generation, searchable_text, tombstone
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
                ON CONFLICT(object_id) DO UPDATE SET
                    resource_id=excluded.resource_id,
                    qihse_artifact_id=excluded.qihse_artifact_id,
                    classification=excluded.classification,
                    sci=excluded.sci,
                    tenant_id=excluded.tenant_id,
                    generation=excluded.generation,
                    searchable_text=excluded.searchable_text,
                    tombstone=excluded.tombstone
                """,
                (
                    object_id,
                    resource_id,
                    artifact_backreference,
                    record_dict["classification"],
                    record_dict["sci"],
                    record_dict["tenant_id"],
                    record_dict["generation"],
                    searchable_text,
                    is_tombstone,
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
                       tenant_id, generation, tombstone
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
            obj_id, res_id, art_id, cls, sci, tenant, gen, tomb = row
            # Enforce exact SCI compartment bitmask
            if sci & ~principal.sci:
                continue
            # Mark candidate as stale if its generation precedes current durable generation
            is_stale = gen < current_generation
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
                )
            )
        return hits
