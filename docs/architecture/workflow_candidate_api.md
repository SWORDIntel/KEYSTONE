# Workflow candidate API

This is a metadata-only projection for applications whose authoritative rows
live in QIHSE. It stores an opaque record UUID, opaque owner UUID, workflow
type, tenant, classification, SCI mask, flags, due deadline, and committed
source sequence.
It accepts no row body, path, source key, search term, or content token.

The native API is in `include/keystone_workflow_index.h`. A trusted projector
calls `keystone_workflow_index_ingest_batch(index, from_exclusive,
through_inclusive, events, count)` for a complete committed interval. Source
transaction indexes within that interval may be sparse. A zero-event batch
advances past commits with no candidate changes. The batch and watermark
become visible together, or neither does. An out-of-order interval, duplicate,
or conflicting record identity is rejected. One batch is capped at 4096
metadata changes; a single committed transaction must fit this bound.

For larger backlogs, `keystone_workflow_index_stage_batch` accepts consecutive
chunks of at most 4096 events, ending each chunk at a committed-sequence
boundary. `keystone_workflow_index_publish` atomically exposes the complete
staged entries and final watermark; `abort_stage` discards them. Pages,
watermark reads, and saves see only the prior published index until publish.
`keystoned` exposes stage, publish, abort, and stage-watermark operations on
the same Unix socket. A staged replay belongs to that connection; other
connections cannot mutate it, and disconnect aborts the volatile stage.
Authenticated idle connections remain open while a projector reads a long
upstream backlog. A partial inbound frame still has a receive timeout, and
the daemon limits concurrent client slots.

`keystone_workflow_index_page` takes workflow type, exact opaque owner, tenant,
clearance, SCI mask, a minimum required committed sequence, a maximum due
deadline, and a `(due_at, UUID)` cursor.
It supports ascending and descending pages. Both directions use the same
exclusive `(due_at, UUID)` continuation; descending starts at the inclusive
maximum deadline and is suitable for newest-first history. It binary seeks
the workflow/owner partition. Security filtering occurs before
the page limit. The first page returns a watermark; follow-up pages must pass
it as `expected_watermark`. A changed or lagging projection returns STALE so
the caller can restart or pause. Every hit must be re-read from the committed
QIHSE view and authorized with the caller's current principal before use.
The descending flag extends the native and Unix-socket query structure. The
daemon, native client library, and application must be released together;
mixed structures fail the socket payload-size check. This source is not yet
deployed.

`keystoned` exposes corresponding `keystoned_client_workflow_apply`,
`keystoned_client_workflow_page`, and
`keystoned_client_workflow_watermark` Unix-socket calls. Workflow IPC requires
a server-registered SO_PEERCRED principal. The standalone daemon enables it
with `-T <tenant>` and must run under a dedicated OS account with its socket
restricted to that account. Apply requires the registered tenant's full
clearance and compartment mask. No workflow endpoint accepts a
client-asserted principal alone.

The daemon accepts up to 32 concurrent local clients. This lets a persistent
projector connection coexist with request readers; additional clients are
rejected until a slot is free. Shutdown wakes and joins active clients before
freeing the workflow index.

The active daemon index is currently memory resident. `create(0)` starts empty;
nonzero initial watermarks are rejected. A daemon restart must replay the
complete authorized source history and reach the required committed watermark
before scan-dependent workers run. A lost apply acknowledgement
requires the projector to check the watermark and reconcile from QIHSE; an
equal or later watermark alone does not prove the contents of the lost batch.
An interrupted staged replay is discarded and restarted from the published
watermark. Stage publication does not itself persist a checkpoint.

An application projector can consume ordered `QIHSE.TXN.READ` pages, decrypt
them inside its own boundary, and send only opaque candidate changes to
KEYSTONE. The existing `KEYSTONE.FEED.PUBLISH` is a
separate federation append and is not a replacement for that committed
transaction read. This API is source-only until the projector and startup
freshness gate are wired and validated on the serving nodes.

# Due work and checkpoints

Workflow entries carry a `due_at` Unix deadline. Pages sort by tenant, type,
owner, deadline, then opaque record ID. `max_due_at` is an inclusive bound;
`UINT64_MAX` disables it. A continuation passes both `next_cursor_due_at` and
`next_cursor` with the exact returned watermark. Updating a deadline reorders
the posting atomically. The caller still re-reads QIHSE state before action.

The index has a versioned, checksummed, atomic save/load API, but daemon
checkpoint loading is deliberately disabled. The trusted projector binds an
identity derived from the first QIHSE committed row and its posting schema
before reading or applying. A schema change must change that identity so an
old index cannot silently omit new posting types. This identifies the genesis
and schema, but cannot prove the entire prefix through a restored checkpoint
watermark after a reset or fork that reused the genesis.
Live daemon restarts therefore replay from zero. Checkpoint restore needs a
committed-prefix proof at the saved watermark before serving any page.
Adding or changing a workflow posting also requires a fresh index and full
replay: an old watermark does not prove that the new posting was projected.

Applications may use separate workflow types and opaque ID domains for each
candidate class. A source transaction must emit at most 4096 candidate events;
larger committed backlogs can be staged in commit-aligned chunks. Record
membership can represent existence alone; content-derived availability or
search terms remain in QIHSE, which performs current checks after candidate
lookup. Candidate pages contain no source record content.

The local application source also uses one opaque posting for each canonical
room message. Its due value is the message ID, allowing descending pages for
recent history and ascending pages for membership backfill. Membership,
message locators, MLS roster boundaries, and SCI remain QIHSE checks. A
schema-bound full replay and explicit operator activation are required before
these reads serve traffic; the source is not deployed.
