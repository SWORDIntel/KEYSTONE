# Scoped AIMEM vector candidate engine

`include/keystone_aimem_vector.h` defines a distinct candidate-only API; it does
not reuse the 64-dimensional incident engine or the metadata-only workflow
index. A builder supplies full 16-byte UUIDs, canonical sequence (zero for
pre-sequence records), content digest, tenant/owner, hierarchy scope, kind,
source labels and normalized-query-compatible vectors. The model SHA-256 must
bind weights and all preprocessing. The builder rejects duplicate UUIDs,
nonfinite/zero vectors and malformed scopes, then seals one immutable generation
at an explicit source watermark. Queries require a matching model and sufficient
watermark. They prefilter tenant, hierarchy, labels, kind and a caller-provided
live visibility predicate **before** bounded exact cosine ranking. A monotonic
deadline fails without returning partial hits. The sealed object supports
concurrent readers; callers must not seal while another thread is querying.

This is a building block, **not** the complete QIHSE integration. The QIHSE
process must use an authenticated, crash-safe source and publish a complete
immutable generation before invoking it. The visibility predicate must apply
current grants; every returned UUID must be re-read and re-authorized through
canonical QIHSE before disclosure. No result contains memory text. Embeddings
and index metadata remain sensitive derived data and require a protected,
model-versioned durable projection before production use. Stale, missing or
corrupt generations are errors, not empty results or synchronous rebuilds.

Run `make bin/test_aimem_vector` and `./bin/test_aimem_vector`. The test covers
scope/tenant/SCI/classification, model/watermark fencing, duplicate UUIDs,
invalid vectors, deadlines, concurrent readers and failed visibility callbacks.
The SIMD kernel dispatches AVX2/FMA at runtime with a scalar fallback. Neither
search latency nor indexed hybrid performance is claimed until QIHSE's worker,
backfill, recovery, authorization and live performance gates pass.
