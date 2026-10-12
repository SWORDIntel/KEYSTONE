/*
 * ks_overflow_probe.c — offline reproduction + regression battery for the
 * KEYSTONE temporal-index load-resume heap overflow (upstream 0ae7b83).
 *
 * Usage: ks_overflow_probe <path-to-libkeystone.so> <workdir> [skip-overflow]
 *
 * Mode "overflow" (default): save a small temporal index (count <= 16), load
 * it, append past the loaded array — exactly the R6b journal-feed resume
 * pattern. Pre-0ae7b83 libs claimed capacity max(count,16) while allocating
 * count slots, so the first append writes past the allocation (valgrind:
 * invalid write of size 8). Also probes the zero-entry file (pre-fix: NULL
 * array with capacity 16 -> first append writes through NULL).
 *
 * The regression battery runs regardless: temporal save/load/append/query,
 * exact-index ops + save/load, federation ingest/submit/dedup/checkpoint,
 * record serialize/deserialize CRC (incl. tamper rejection and the sci
 * compartment round-trip from criterion 8).
 *
 * dlopen-based so the same binary can target any lib build (backup, deployed,
 * fresh) without relinking.
 */

#include "keystone_temporal.h"
#include "keystone_exact_index.h"
#include "keystone_federation.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ types */

typedef keystone_temporal_index_t* (*t_create_fn)(size_t);
typedef void (*t_destroy_fn)(keystone_temporal_index_t*);
typedef int (*t_append_fn)(keystone_temporal_index_t*, const keystone_temporal_entry_t*);
typedef int (*t_ingest_rec_fn)(keystone_temporal_index_t*, const keystone_federation_record_t*, uint64_t);
typedef size_t (*t_count_fn)(const keystone_temporal_index_t*);
typedef size_t (*t_query_range_fn)(const keystone_temporal_index_t*, const keystone_hlc_t*, const keystone_hlc_t*, keystone_temporal_entry_t*, size_t);
typedef size_t (*t_query_obj_fn)(const keystone_temporal_index_t*, const keystone_uuid_t*, const keystone_hlc_t*, const keystone_hlc_t*, keystone_temporal_entry_t*, size_t);
typedef bool (*t_watermark_fn)(const keystone_temporal_index_t*, keystone_hlc_t*, size_t*);
typedef size_t (*t_scan_rev_fn)(const keystone_temporal_index_t*, const keystone_hlc_t*, keystone_temporal_entry_t*, size_t);
typedef size_t (*t_tomb_count_fn)(const keystone_temporal_index_t*);
typedef int (*t_save_fn)(const keystone_temporal_index_t*, const char*);
typedef int (*t_load_fn)(keystone_temporal_index_t**, const char*);

typedef keystone_exact_index_t* (*x_create_fn)(size_t);
typedef void (*x_destroy_fn)(keystone_exact_index_t*);
typedef int (*x_upsert_fn)(keystone_exact_index_t*, const keystone_exact_entry_t*);
typedef int (*x_ingest_rec_fn)(keystone_exact_index_t*, const keystone_federation_record_t*, uint64_t);
typedef int (*x_lookup_fn)(const keystone_exact_index_t*, const keystone_uuid_t*, keystone_exact_entry_t*);
typedef bool (*x_contains_fn)(const keystone_exact_index_t*, const keystone_uuid_t*);
typedef size_t (*x_count_fn)(const keystone_exact_index_t*);
typedef size_t (*x_active_fn)(const keystone_exact_index_t*);
typedef int (*x_save_fn)(const keystone_exact_index_t*, const char*);
typedef int (*x_load_fn)(keystone_exact_index_t**, const char*);

typedef keystone_uuid_t (*u_nil_fn)(void);
typedef int (*ser_fn)(const keystone_federation_record_t*, void*, size_t, size_t*);
typedef int (*deser_fn)(const void*, size_t, keystone_federation_record_t*);
typedef keystone_federation_ingest_t* (*ing_create_fn)(const keystone_ingest_config_t*);
typedef void (*ing_destroy_fn)(keystone_federation_ingest_t*);
typedef keystone_ingest_status_t (*ing_submit_fn)(keystone_federation_ingest_t*, const keystone_federation_record_t*);
typedef int (*ing_stats_fn)(const keystone_federation_ingest_t*, keystone_ingest_stats_t*);
typedef int (*ing_save_cp_fn)(const keystone_federation_ingest_t*, const char*);
typedef int (*ing_load_cp_fn)(keystone_federation_ingest_t*, const char*);
typedef bool (*ing_tomb_fn)(const keystone_federation_ingest_t*, const keystone_uuid_t*);

static struct {
    t_create_fn   t_create;   t_destroy_fn t_destroy;  t_append_fn  t_append;
    t_ingest_rec_fn t_ingest_rec; t_count_fn t_count;   t_query_range_fn t_qrange;
    t_query_obj_fn t_qobj;    t_watermark_fn t_wm;     t_scan_rev_fn t_srev;
    t_tomb_count_fn t_tombs;  t_save_fn    t_save;     t_load_fn    t_load;
    x_create_fn   x_create;   x_destroy_fn x_destroy;  x_upsert_fn  x_upsert;
    x_ingest_rec_fn x_ingest_rec; x_lookup_fn x_lookup; x_contains_fn x_contains;
    x_count_fn    x_count;    x_active_fn  x_active;   x_save_fn    x_save;
    x_load_fn     x_load;
    u_nil_fn      u_nil;
    ser_fn        serialize;  deser_fn     deserialize;
    ing_create_fn ing_create; ing_destroy_fn ing_destroy; ing_submit_fn ing_submit;
    ing_stats_fn  ing_stats;  ing_save_cp_fn ing_save_cp; ing_load_cp_fn ing_load_cp;
    ing_tomb_fn   ing_tomb;
} F;

static int failures = 0;

static void* must_sym(void* h, const char* name) {
    void* p = dlsym(h, name);
    if (!p) {
        fprintf(stderr, "dlsym(%s) failed: %s\n", name, dlerror());
        exit(2);
    }
    return p;
}

#define SYM(field, name) F.field = (typeof(F.field))must_sym(h, name)
#define SYM_OPT(field, name) F.field = (typeof(F.field))dlsym(h, name)

static void load_lib(const char* path) {
    void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "dlopen(%s) failed: %s\n", path, dlerror());
        exit(2);
    }
    SYM(t_create, "keystone_temporal_index_create");
    SYM(t_destroy, "keystone_temporal_index_destroy");
    SYM(t_append, "keystone_temporal_index_append");
    SYM(t_ingest_rec, "keystone_temporal_index_ingest_record");
    SYM(t_count, "keystone_temporal_index_count");
    SYM(t_qrange, "keystone_temporal_index_query_range");
    SYM(t_qobj, "keystone_temporal_index_query_object");
    SYM_OPT(t_wm, "keystone_temporal_index_watermark");       /* post-870e1b4 */
    SYM_OPT(t_srev, "keystone_temporal_index_scan_reverse");  /* post-870e1b4 */
    SYM_OPT(t_tombs, "keystone_temporal_index_tombstone_count");
    SYM(t_save, "keystone_temporal_index_save");
    SYM(t_load, "keystone_temporal_index_load");
    SYM(x_create, "keystone_exact_index_create");
    SYM(x_destroy, "keystone_exact_index_destroy");
    SYM(x_upsert, "keystone_exact_index_upsert");
    SYM(x_ingest_rec, "keystone_exact_index_ingest_record");
    SYM(x_lookup, "keystone_exact_index_lookup");
    SYM(x_contains, "keystone_exact_index_contains");
    SYM(x_count, "keystone_exact_index_count");
    SYM(x_active, "keystone_exact_index_active_count");
    SYM(x_save, "keystone_exact_index_save");
    SYM(x_load, "keystone_exact_index_load");
    SYM(u_nil, "keystone_uuid_nil");
    SYM(serialize, "keystone_record_serialize");
    SYM(deserialize, "keystone_record_deserialize");
    SYM(ing_create, "keystone_federation_ingest_create");
    SYM(ing_destroy, "keystone_federation_ingest_destroy");
    SYM(ing_submit, "keystone_federation_ingest_submit");
    SYM(ing_stats, "keystone_federation_get_stats");
    SYM(ing_save_cp, "keystone_federation_save_checkpoint");
    SYM(ing_load_cp, "keystone_federation_load_checkpoint");
    SYM(ing_tomb, "keystone_federation_is_tombstoned");
}

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS %s\n", msg); } \
    else { printf("  FAIL %s\n", msg); failures++; } \
} while (0)

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static keystone_temporal_entry_t make_tentry(uint64_t ms, uint32_t logical, uint32_t flags) {
    keystone_temporal_entry_t e;
    memset(&e, 0, sizeof(e));
    e.hlc.physical_ms = ms;
    e.hlc.logical = logical;
    e.hlc.node_id = 0xBEEF;
    for (int i = 0; i < 16; i++) e.event_id.bytes[i] = (uint8_t)(0x10 + i + logical);
    for (int i = 0; i < 16; i++) e.object_id.bytes[i] = (uint8_t)(0xA0 + i);
    e.event_type = KEYSTONE_OBJ_LOG_STREAM;
    e.tenant_id = 7;
    e.classification = KEYSTONE_CLASSIFICATION_OPS;
    e.flags = flags;
    e.source_generation = 100 + logical;
    e.payload_offset = logical * 64;
    return e;
}

static keystone_federation_record_t make_record(uint64_t gen, uint32_t sci_bits, uint64_t ms) {
    keystone_federation_record_t r;
    memset(&r, 0, sizeof(r));
    for (int i = 0; i < 16; i++) r.source_object_id.bytes[i] = (uint8_t)(0x30 + i);
    for (int i = 0; i < 16; i++) r.source_event_id.bytes[i] = (uint8_t)(0x40 + i + gen);
    for (int i = 0; i < 16; i++) r.source_node_id.bytes[i] = (uint8_t)(0x50 + i);
    r.source_generation = gen;
    r.fencing_epoch = 1;
    r.source_hlc.physical_ms = ms;
    r.source_hlc.logical = (uint32_t)gen;
    r.source_hlc.node_id = 0x1234;
    r.tenant_id = 3;
    r.classification = KEYSTONE_CLASSIFICATION_OPS;
    r.object_type = KEYSTONE_OBJ_LOG_STREAM;
    r.flags = KEYSTONE_RECORD_FLAG_AUDIT;
    static const char payload[] = "probe-payload-0123456789abcdef";
    r.payload = payload;
    r.payload_len = sizeof(payload) - 1;
    r.sci = sci_bits;
    return r;
}

/* ------------------------------------------------------------ probe: overflow */

static void probe_overflow(const char* dir, int probe_zero) {
    char path[512];
    snprintf(path, sizeof(path), "%s/temporal_probe.index", dir);

    /* Stage 1: create small index, append 8 entries, save. */
    keystone_temporal_index_t* idx = F.t_create(8);
    uint64_t base = now_ms() - 100000;
    for (uint32_t i = 0; i < 8; i++) {
        keystone_temporal_entry_t e = make_tentry(base + i * 1000, i, KEYSTONE_RECORD_FLAG_EVENT);
        if (F.t_append(idx, &e) != 0) {
            printf("  FAIL append %u during staging\n", i);
            failures++;
        }
    }
    if (F.t_save(idx, path) != 0) {
        printf("  FAIL save staged index\n");
        failures++;
    }
    F.t_destroy(idx);

    /* Stage 2: LOAD it, then append — the journal-feed resume pattern. */
    keystone_temporal_index_t* loaded = NULL;
    if (F.t_load(&loaded, path) != 0 || !loaded) {
        printf("  FAIL load staged index\n");
        failures++;
        return;
    }
    printf("  loaded count=%zu\n", F.t_count(loaded));
    for (uint32_t i = 8; i < 24; i++) {   /* writes indices 8..23 past an 8-slot array */
        keystone_temporal_entry_t e = make_tentry(base + i * 1000, i, KEYSTONE_RECORD_FLAG_EVENT);
        if (F.t_append(loaded, &e) != 0) {
            printf("  FAIL post-load append %u\n", i);
            failures++;
        }
    }
    CHECK(F.t_count(loaded) == 24, "post-load appends all recorded (count==24)");
    if (F.t_save(loaded, path) != 0) {
        printf("  FAIL save grown index\n");
        failures++;
    }
    F.t_destroy(loaded);

    /* Stage 3: reload the grown file and confirm integrity. */
    loaded = NULL;
    if (F.t_load(&loaded, path) != 0 || !loaded) {
        printf("  FAIL reload grown index\n");
        failures++;
        return;
    }
    CHECK(F.t_count(loaded) == 24, "reloaded grown index count==24");
    F.t_destroy(loaded);

    if (probe_zero) {
        /* Zero-entry file: pre-0ae7b83 loaded entries=NULL with capacity 16
         * -> the first append wrote through NULL. */
        snprintf(path, sizeof(path), "%s/temporal_zero.index", dir);
        keystone_temporal_index_t* empty = F.t_create(4);
        if (F.t_save(empty, path) != 0) {
            printf("  FAIL save empty index\n");
            failures++;
        }
        F.t_destroy(empty);
        keystone_temporal_index_t* zloaded = NULL;
        if (F.t_load(&zloaded, path) != 0 || !zloaded) {
            printf("  FAIL load empty index\n");
            failures++;
            return;
        }
        keystone_temporal_entry_t e = make_tentry(now_ms(), 0, KEYSTONE_RECORD_FLAG_EVENT);
        int rc = F.t_append(zloaded, &e);
        CHECK(rc == 0 && F.t_count(zloaded) == 1, "append to zero-entry loaded index");
        F.t_destroy(zloaded);
    }
}

/* --------------------------------------------------------- battery: temporal */

static void battery_temporal(const char* dir) {
    printf("battery: temporal index\n");
    char path[512];
    snprintf(path, sizeof(path), "%s/temporal_batt.index", dir);

    keystone_temporal_index_t* idx = F.t_create(0); /* default 4096 capacity */
    CHECK(idx != NULL, "temporal create(0)");

    /* 450 entries at production scale, one tombstone in the middle. */
    uint64_t base = now_ms() - 1000000;
    int ok = 1;
    for (uint32_t i = 0; i < 450; i++) {
        uint32_t flags = (i == 200) ? KEYSTONE_RECORD_FLAG_TOMBSTONE : KEYSTONE_RECORD_FLAG_EVENT;
        keystone_temporal_entry_t e = make_tentry(base + i * 100, i, flags);
        if (F.t_append(idx, &e) != 0) ok = 0;
    }
    CHECK(ok && F.t_count(idx) == 450, "450 appends recorded");

    /* Out-of-order arrival lands in sorted position. */
    keystone_temporal_entry_t mid = make_tentry(base + 150 * 100 + 50, 9999, KEYSTONE_RECORD_FLAG_EVENT);
    CHECK(F.t_append(idx, &mid) == 0, "out-of-order append accepted");

    keystone_hlc_t lo = { base, 0, 0 };
    keystone_hlc_t hi = { base + 200 * 100, 0xFFFFFFFFu, 0xFFFFFFFFu };
    keystone_temporal_entry_t out[512];
    size_t n = F.t_qrange(idx, &lo, &hi, out, 512);
    CHECK(n == 202, "range query matches [0,200] inclusive");
    int sorted = 1;
    for (size_t i = 1; i < n; i++) {
        if (out[i].hlc.physical_ms < out[i - 1].hlc.physical_ms) sorted = 0;
    }
    CHECK(sorted, "range results ascending by HLC");

    keystone_uuid_t obj = make_tentry(0, 0, 0).object_id;
    n = F.t_qobj(idx, &obj, NULL, NULL, out, 512);
    CHECK(n == 451, "object query sees full timeline");

    keystone_hlc_t wm_hlc; size_t wm_count = 0;
    CHECK(F.t_wm(idx, &wm_hlc, &wm_count) && wm_count == 451, "watermark count==451");

    n = F.t_srev(idx, NULL, out, 10);
    /* newest entry is i=449 (450 appends, indices 0..449) */
    CHECK(n == 10 && out[0].hlc.physical_ms == base + 449 * 100, "reverse scan newest-first");

    CHECK(F.t_tombs(idx) == 1, "tombstone count==1");

    CHECK(F.t_save(idx, path) == 0, "temporal save");
    F.t_destroy(idx);

    keystone_temporal_index_t* rt = NULL;
    CHECK(F.t_load(&rt, path) == 0 && rt != NULL, "temporal load roundtrip");
    CHECK(F.t_count(rt) == 451, "roundtrip count==451");
    CHECK(F.t_wm(rt, &wm_hlc, &wm_count) && wm_count == 451, "loaded watermark count==451");

    /* append past a freshly-loaded index at prod scale (the deployed path) */
    keystone_temporal_entry_t e2 = make_tentry(base + 1000000, 50000, KEYSTONE_RECORD_FLAG_EVENT);
    CHECK(F.t_append(rt, &e2) == 0 && F.t_count(rt) == 452, "append to loaded 451-entry index");

    /* ingest_record convenience path */
    keystone_federation_record_t r = make_record(77, 0, base + 2000000);
    CHECK(F.t_ingest_rec(rt, &r, 12345) == 0, "temporal ingest_record");
    CHECK(F.t_count(rt) == 453, "ingest_record appended");
    F.t_destroy(rt);
}

/* ----------------------------------------------------------- battery: exact */

static void battery_exact(const char* dir) {
    printf("battery: exact index\n");
    char path[512];
    snprintf(path, sizeof(path), "%s/exact_batt.index", dir);

    keystone_exact_index_t* idx = F.x_create(64);
    CHECK(idx != NULL, "exact create(64)");

    uint64_t ms = now_ms();
    keystone_exact_entry_t ent;
    memset(&ent, 0, sizeof(ent));
    for (int i = 0; i < 16; i++) ent.resource_id.bytes[i] = (uint8_t)(0x60 + i);
    for (int i = 0; i < 16; i++) ent.latest_event_id.bytes[i] = (uint8_t)(0x70 + i);
    ent.latest_generation = 42;
    ent.fencing_epoch = 1;
    ent.latest_hlc.physical_ms = ms;
    ent.latest_hlc.logical = 5;
    ent.latest_hlc.node_id = 0x99;
    ent.classification = KEYSTONE_CLASSIFICATION_OPS;

    CHECK(F.x_upsert(idx, &ent) == 0, "exact upsert");

    keystone_federation_record_t r = make_record(43, 0, ms + 1000);
    CHECK(F.x_ingest_rec(idx, &r, 777) == 0, "exact ingest_record");
    CHECK(F.x_count(idx) == 2, "exact count==2");

    keystone_exact_entry_t got;
    keystone_federation_record_t r2 = make_record(43, 0, ms + 1000);
    CHECK(F.x_lookup(idx, &r2.source_object_id, &got) == 0, "exact lookup (ingest_record resource)");
    CHECK(F.x_contains(idx, &ent.resource_id), "exact contains");

    CHECK(F.x_save(idx, path) == 0, "exact save");
    F.x_destroy(idx);

    keystone_exact_index_t* rt = NULL;
    CHECK(F.x_load(&rt, path) == 0 && rt != NULL, "exact load roundtrip");
    CHECK(F.x_count(rt) == 2, "exact roundtrip count==2");
    CHECK(F.x_contains(rt, &ent.resource_id), "exact roundtrip contains");
    F.x_destroy(rt);
}

/* ------------------------------------------------------- battery: federation */

static void battery_federation(const char* dir) {
    printf("battery: federation ingest + checkpoint\n");
    char cp[512];
    snprintf(cp, sizeof(cp), "%s/engine.checkpoint", dir);

    keystone_ingest_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.enable_concurrency = 1;

    keystone_federation_ingest_t* eng = F.ing_create(&cfg);
    CHECK(eng != NULL, "ingest engine create");

    uint64_t ms = now_ms();
    for (uint64_t g = 1; g <= 5; g++) {
        keystone_federation_record_t r = make_record(g, 0, ms + g);
        keystone_ingest_status_t st = F.ing_submit(eng, &r);
        if (st != KEYSTONE_INGEST_OK) {
            printf("  FAIL submit gen %llu -> %d\n", (unsigned long long)g, st);
            failures++;
        }
    }
    keystone_ingest_stats_t st;
    CHECK(F.ing_stats(eng, &st) == 0 && st.records_ingested == 5, "5 records ingested");

    /* duplicate suppression */
    keystone_federation_record_t dup = make_record(3, 0, ms + 3);
    CHECK(F.ing_submit(eng, &dup) == KEYSTONE_INGEST_DUPLICATE, "duplicate suppressed");

    /* Older generation + NEW event id: accepted at HEAD — the engine only
     * advances watermarks monotonically; staleness is a consumer-side
     * comparison (see the feed's --verify generation check). */
    keystone_federation_record_t stale = make_record(2, 0, ms + 2);
    stale.source_event_id.bytes[15] ^= 0xFF; /* distinct event id, older generation */
    CHECK(F.ing_submit(eng, &stale) == KEYSTONE_INGEST_OK, "old-generation new-event accepted");
    {
        keystone_ingest_stats_t wst;
        CHECK(F.ing_stats(eng, &wst) == 0 && wst.current_generation == 5,
              "generation watermark not regressed by old record");
    }

    /* tombstone flow (checkpoint saved AFTER so resume sees the tombstone) */
    keystone_federation_record_t tomb = make_record(6, 0, ms + 6);
    tomb.flags |= KEYSTONE_RECORD_FLAG_TOMBSTONE;
    CHECK(F.ing_submit(eng, &tomb) == KEYSTONE_INGEST_TOMBSTONE_APPLIED, "tombstone applied");
    CHECK(F.ing_tomb(eng, &tomb.source_object_id), "object tombstoned");

    CHECK(F.ing_save_cp(eng, cp) == 0, "checkpoint save");

    F.ing_destroy(eng);

    /* resume from checkpoint */
    keystone_federation_ingest_t* eng2 = F.ing_create(&cfg);
    CHECK(F.ing_load_cp(eng2, cp) == 0, "checkpoint load");
    keystone_ingest_stats_t st2;
    CHECK(F.ing_stats(eng2, &st2) == 0, "stats after resume");
    CHECK(st2.current_generation == 6, "resume generation==6");
    CHECK(st2.tombstones_active == 1, "resume tombstones active==1");
    F.ing_destroy(eng2);
}

/* -------------------------------------------------- battery: wire CRC + sci */

static void battery_wire(void) {
    printf("battery: record serialize/deserialize CRC\n");
    unsigned char buf[4096];
    size_t written = 0;

    keystone_federation_record_t r = make_record(9, 0x0000005Au, now_ms());
    CHECK(F.serialize(&r, buf, sizeof(buf), &written) == 0 && written > 0,
          "serialize ok");

    keystone_federation_record_t out;
    memset(&out, 0, sizeof(out));
    CHECK(F.deserialize(buf, written, &out) == 0, "deserialize CRC ok");
    CHECK(out.source_generation == 9 && out.tenant_id == 3, "generation/tenant roundtrip");
    CHECK(memcmp(out.source_object_id.bytes, r.source_object_id.bytes, 16) == 0,
          "object id roundtrip");
    CHECK(out.payload_len == r.payload_len &&
          memcmp(out.payload, r.payload, r.payload_len) == 0, "payload roundtrip");
    CHECK(out.sci == 0x0000005Au, "sci compartment roundtrip (criterion 8)");

    /* tamper -> CRC must reject */
    buf[written / 2] ^= 0x01;
    memset(&out, 0, sizeof(out));
    CHECK(F.deserialize(buf, written, &out) != 0, "tampered envelope rejected");
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <libkeystone.so> <workdir> [skip-overflow|overflow-only]\n", argv[0]);
        return 2;
    }
    const char* lib = argv[1];
    const char* dir = argv[2];
    const char* mode = (argc > 3) ? argv[3] : "";
    int skip_overflow = (strcmp(mode, "skip-overflow") == 0);
    int overflow_only = (strcmp(mode, "overflow-only") == 0);

    load_lib(lib);
    printf("lib loaded: %s\n", lib);

    if (!skip_overflow) {
        printf("probe: load-resume overflow pattern\n");
        probe_overflow(dir, 1);
    }
    if (!overflow_only) {
        battery_temporal(dir);
        battery_exact(dir);
        battery_federation(dir);
        battery_wire();
    }

    printf(failures ? "RESULT: FAIL (%d)\n" : "RESULT: OK (0 failures)\n", failures);
    return failures ? 1 : 0;
}
