# QIHSE feed integration boundary

`python/keystone_qihse_feed` contains three distinct layers; do not treat them
as one staging-only implementation:

- `FeedStager` (`__init__.py`) validates and stages records in memory. It has
  no durable cursor, serving API, or ACK path.
- `DurableFeed` (`durable.py`) persists replay IDs, object state, generation,
  and source cursor in a local SQLite transaction, then sends
  `KEYSTONE.FEED.ACK` after commit.
- `KeystoneAnalysisIndex` and `KeystoneFeedConsumer` (`index.py`) add a SQLite
  search projection in that same transaction and expose candidate search with
  tenant/classification/SCI checks and QIHSE reauthorization for payload reads.

The latter two layers are real source behavior, but they are not KEYSTONE's
native immutable exact/temporal generation publisher or `keystoned` service.
They use a separate SQLite store and do not establish restart-safe publication
through KEYSTONE's native active-generation pointer. Treat this Python package
as a prototype, not as the production DIRECTEYE projection or a canonical
store. QIHSE remains authoritative; do not use this SQLite state as a
DIRECTEYE source of truth.

The caller supplies a QIHSE Python SDK `Controller` that has authenticated
with an explicitly provisioned, tenant-scoped indexer identity. Alternatively,
`FeedStager.connect` constructs it with explicit username/password; plaintext
RESP is accepted only for loopback, and remote connections require explicit
QKP identity/trust configuration. No credentials are embedded or read from the
environment. The server retains the authority to refuse `OPEN` or filter
delivery. The stager checks tenant, clearance, SCI, and resource prefix again
before retaining any bytes.

Unlike the current SDK `FeedEvent` convenience decoder, `FeedStager` validates
the raw 16-element RESP array with exact per-field types, integer widths,
canonical UUIDs, UTF-8, and QIHSE's 256 KiB body cap. It retains tombstone
metadata, security-sensitive flags, origin/event/object IDs, source stamps,
and live payloads. Tombstones discard their own and the replaced payload bytes
after dedup fingerprinting. Retained payloads have an aggregate 32 MiB ceiling
(lowerable by caller), charged after replacement/deletion, in addition to the
object and event count ceilings. Identical event-ID replay is idempotent;
conflicting reuse, stale *per-object* fencing epochs, stale object
replacements, unauthorized scope, and cap exhaustion stop ingest. The journal
envelope's fencing epoch is not assumed globally monotonic across objects.
Resurrection after a tombstone requires both newer generation and newer HLC.
The SQLite-backed layers add persistence and a search projection, but do not
change those limits into KEYSTONE-native generation publication.

## DIRECTEYE-QIHSE integration status

QIHSE's authenticated native RESP/QKP API and admin CLI are separate from its
HTTP route layer. The `KEYSTONE.FEED.*` commands are a journal feed, not a
watch on arbitrary QIHSE KV/canonical-record writes. QIHSE now exposes dedicated
`DIRECTEYE.RECORD.GET`, `DIRECTEYE.RECORD.MUTATE`, `DIRECTEYE.FEED.READ`, and
`DIRECTEYE.FEED.ACK` commands over authenticated RESP; the mutation/outbox and
read paths are tested through authenticated dispatch and an isolated sealed QKP1
client/server fixture. The DIRECTEYE Python backend has an injected-session QKP
adapter for record reads, mutations, feed reads, and ACKs; it does not create a
transport or provide identity/configuration. `DatasetRepository` now accepts
that injected native client, derives stable operation IDs from the existing
observation marker, and reconciles ambiguous mutation outcomes by authenticated
read-back without a local cache or caller tenant. KEYSTONE has an optional native
QKP consumer (`KEYSTONE_ENABLE_DIRECTEYE_QIHSE=1`) and an isolated sealed-QKP
integration fixture covering canonical upsert, replacement, deletion, projection,
durable ACK, and restart. The same fixture now reads the QKP-projected payload
through a live local `keystoned` IPC server and verifies tenant/classification/SCI
denial and tombstone denial against the mapping sidecar. `keystoned` can serve
the persisted projection through its DIRECTEYE record-get IPC when started with
`--directeye-publication <dir>`. The existing analysis-artifact writer's
`kp14.analysis.*` events and `KEYSTONE.FEED.*` journal remain separate.

For a production projection, establish the source event contract first:
DIRECTEYE record identity and schema/version, create/update/delete semantics,
source generation and cursor, tenant/classification/SCI, and bounded payload.
DIRECTEYE mutations are authorized against the authenticated principal and the
canonical row's tenant/classification/SCI; the provisioned index identity is
read-only and cannot mutate source records. QIHSE filters feed records to that
identity's tenant, clearance, and SCI. A consumer must preserve those labels and
tombstones and must not interpret caller-supplied identity as authentication.

KEYSTONE's native publication primitives write exact and temporal indexes,
manifests containing a source cursor, and an active pointer in publication
order (`keystone_publication_publish()` in
`src/federation/keystone_manifest.c`). A sidecar-aware variant now also syncs a
checksummed generation-bound mapping sidecar before the active pointer and
rejects generations whose sidecar is absent/corrupt on reopen. The native
`keystone_directeye_project_page()` core now applies validated typed events to
exact/temporal indexes and the mapping sidecar, preserves raw payload and
security/source metadata, uses source commit time for deterministic HLC ordering,
and verifies reopened generation/cursor before reporting success. A bounded C
JSON decoder rejects duplicate keys and missing timestamps while preserving the
exact record-object byte span. The optional QKP consumer invokes the decoder and
projector, reopens and verifies publication before issuing the matching ACK, and
returns a distinct result if publication succeeds but ACK fails. Isolated native
tests cover sealed QKP consumption, replacement, tombstone, authorized read-back,
and restart. `keystoned` now has a mapping-sidecar-backed record-get IPC route;
its publication handle is configured with `--directeye-publication <dir>`. A
failure after pointer rename but before parent-directory sync is not a successful
commit; recover from the published watermark and safely replay. Do not substitute
the Python SQLite transaction for this native publication contract.

Remaining production gates:

1. Wire the application runtime to its scoped authenticated writer session and
   provision the tenant/security-scoped read-only index identity. The Python
   adapter intentionally receives an injected session; it does not create or
   persist credentials. `KEYSTONE.FEED.*` remains unrelated analysis-artifact
   history, not the DIRECTEYE source feed.
2. Integrate the native consumer and publication directory with the operational
   KEYSTONE service lifecycle; test daemon restart and ACK-failure recovery. The
   QKP-to-daemon IPC fixture uses the real local server API, while the
   `--directeye-publication` process lifecycle check currently starts with an
   empty disposable publication.
3. Define and validate the bounded domain projection schema at its publisher;
   the envelope parser enforces JSON structure and limits but not a domain
   schema.
4. Add QKP denial, ACK-failure, source-timestamp-regression-through-consumer,
   and corrupt-sidecar recovery tests. Existing Python adapter tests use raw reply
   fixtures and a fake authenticated controller boundary.
5. Specify snapshot bootstrap and compaction recovery for feeds too large for a
   full replay. All current QKP evidence is isolated and local; no production
   cutover has occurred.

Focused test: from the KEYSTONE repository root run
`python3 -m unittest discover -s python/tests -p test_qihse_feed_ingest.py -v`.
