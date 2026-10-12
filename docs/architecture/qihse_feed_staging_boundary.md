# QIHSE feed staging boundary (non-serving)

`python/keystone_qihse_feed` is a bounded, independent Python package that
stages `KEYSTONE.FEED.NEXT` records without publishing a KEYSTONE index. It
does not replace QIHSE as authority, expose a search API, persist records or
cursor, or send `KEYSTONE.FEED.ACK`. It is deliberately unsuitable for a
long-running production indexer until the gates below are closed.

The caller supplies a QIHSE Python SDK `Controller` that has authenticated
with an explicitly provisioned, tenant-scoped indexer identity. Alternatively,
`FeedStager.connect` constructs it with explicit username/password; plaintext
RESP is accepted only for loopback, and remote connections require explicit
QKP identity/trust configuration. No credentials are embedded or read from the
environment. The server retains the authority to refuse `OPEN` or filter
delivery. The stager checks tenant, clearance, SCI, and resource prefix again
before retaining any bytes.

Unlike the current SDK `FeedEvent` convenience decoder, the stager validates
the raw 16-element RESP array with exact per-field types, integer widths,
canonical UUIDs, UTF-8, and QIHSE's 256 KiB body cap. It retains tombstone
metadata, security-sensitive flags, origin/event/object IDs, source stamps,
and live payloads without promoting any record to a public search surface.
Tombstones discard their own and the replaced payload bytes after dedup
fingerprinting. Retained payloads have an aggregate 32 MiB ceiling (lowerable
by caller), charged after replacement/deletion, in addition to the object and
event count ceilings. Identical event-ID replay is idempotent; conflicting
reuse, stale *per-object* fencing epochs, stale object replacements,
unauthorized scope, and cap exhaustion stop ingest. The journal envelope's
fencing epoch is not assumed globally monotonic across objects. Resurrection
after a tombstone requires both newer generation and newer HLC.

Pending production gates:

1. Define a bounded artifact-manifest schema and validate publication at the
   QIHSE source. The stager currently treats payload bytes as opaque.
2. Publish an exact/temporal KEYSTONE generation and cursor atomically with
   crash-safe fsync/rename/recovery, then ACK only that durable watermark.
   Until then every cold start replays from cursor zero and no ACK is sent.
3. Make query-time SCI/classification enforcement and tombstone filtering
   complete before any classified/untrusted analyst search endpoint is built.
4. Add a journal-backed integration fixture exercising reconnect, replay,
   deletion, replacement, authorization denial, and corruption. Current unit
   tests use raw reply fixtures and a fake authenticated controller boundary.
5. Decide snapshot bootstrap and compaction recovery semantics for feeds too
   large for a full replay; this bounded memory stager refuses overflow.

Focused test: from the KEYSTONE repository root run
`python3 -m unittest discover -s python/tests -p test_qihse_feed_ingest.py -v`.
