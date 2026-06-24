#include "../include/dsmil_hash_indexer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* FNV-1a 64-bit string hash */
static int64_t dsmil_hash_string(const char* str, size_t len) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)str[i];
        h *= 0x100000001b3ULL;
    }
    return (int64_t)h;
}

/* Internal pair for dual-array sorting */
typedef struct {
    int64_t hash;
    uint64_t offset;
} hash_sort_pair_t;

static int compare_pairs(const void* a, const void* b) {
    int64_t ha = ((const hash_sort_pair_t*)a)->hash;
    int64_t hb = ((const hash_sort_pair_t*)b)->hash;
    return (ha < hb) ? -1 : (ha > hb ? 1 : 0);
}

dsmil_hash_index_t* dsmil_hash_index_create(size_t initial_capacity) {
    if (initial_capacity == 0) initial_capacity = 1024;
    dsmil_hash_index_t* idx = calloc(1, sizeof(dsmil_hash_index_t));
    if (!idx) return NULL;
    
    idx->hashes = malloc(initial_capacity * sizeof(int64_t));
    idx->offsets = malloc(initial_capacity * sizeof(uint64_t));
    idx->anchor_table = keystone_anchor_table_create();
    
    if (!idx->hashes || !idx->offsets || !idx->anchor_table) {
        dsmil_hash_index_destroy(idx);
        return NULL;
    }
    
    idx->capacity = initial_capacity;
    idx->count = 0;
    idx->is_sorted = 0;
    return idx;
}

void dsmil_hash_index_destroy(dsmil_hash_index_t* idx) {
    if (!idx) return;
    free(idx->hashes);
    free(idx->offsets);
    if (idx->anchor_table) keystone_anchor_table_destroy(idx->anchor_table);
    free(idx);
}

int dsmil_hash_index_add(dsmil_hash_index_t* idx, const char* str, size_t len, uint64_t byte_offset) {
    if (!idx || !str) return -1;
    
    if (idx->count >= idx->capacity) {
        size_t new_cap = idx->capacity * 2;
        int64_t* new_h = realloc(idx->hashes, new_cap * sizeof(int64_t));
        uint64_t* new_o = realloc(idx->offsets, new_cap * sizeof(uint64_t));
        if (!new_h || !new_o) {
            /* If realloc fails, preserve existing data */
            if (new_h) idx->hashes = new_h;
            if (new_o) idx->offsets = new_o;
            return -1; 
        }
        idx->hashes = new_h;
        idx->offsets = new_o;
        idx->capacity = new_cap;
    }
    
    idx->hashes[idx->count] = dsmil_hash_string(str, len);
    idx->offsets[idx->count] = byte_offset;
    idx->count++;
    idx->is_sorted = 0;
    return 0;
}

int dsmil_hash_index_finalize(dsmil_hash_index_t* idx) {
    if (!idx || idx->count == 0) return 0;
    if (idx->is_sorted) return 0;
    
    /* Allocate temp array of pairs to sort together */
    hash_sort_pair_t* pairs = malloc(idx->count * sizeof(hash_sort_pair_t));
    if (!pairs) return -1;
    
    for (size_t i = 0; i < idx->count; i++) {
        pairs[i].hash = idx->hashes[i];
        pairs[i].offset = idx->offsets[i];
    }
    
    /* Sort the packed struct array */
    qsort(pairs, idx->count, sizeof(hash_sort_pair_t), compare_pairs);
    
    /* Scatter back to Columnar/SoA layout for KEYSTONE SIMD efficiency */
    for (size_t i = 0; i < idx->count; i++) {
        idx->hashes[i] = pairs[i].hash;
        idx->offsets[i] = pairs[i].offset;
    }
    
    free(pairs);
    idx->is_sorted = 1;
    
    /* Pre-warm the KEYSTONE anchor table */
    keystone_config_t cfg;
    keystone_config_init(&cfg, KEYSTONE_WORKLOAD_IDS);
    /* Run a dummy search to build the anchor table internally */
    keystone_search_enhanced(idx->hashes, idx->count, idx->hashes[idx->count/2], idx->anchor_table, &cfg);
    
    return 0;
}

keystone_result_t dsmil_hash_index_search(dsmil_hash_index_t* idx, const char* query_str, uint64_t* out_offset) {
    if (!idx || !query_str || !idx->is_sorted || idx->count == 0) return KEYSTONE_NOT_FOUND;
    
    int64_t target_hash = dsmil_hash_string(query_str, strlen(query_str));
    
    keystone_config_t cfg;
    keystone_config_init(&cfg, KEYSTONE_WORKLOAD_IDS);
    
    keystone_result_t result = keystone_search_enhanced(
        idx->hashes, idx->count, target_hash, idx->anchor_table, &cfg
    );
    
    if (result != KEYSTONE_NOT_FOUND && out_offset) {
        *out_offset = idx->offsets[result];
    }
    
    return result;
}
