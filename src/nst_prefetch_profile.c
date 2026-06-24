/* NST Prefetch-Distance Profiler & Adaptive Warm-Up Scheduler */

#include "nst_prefetch_profile.h"
#include <stdlib.h>

#define PREFETCH_HISTORY_SIZE 256

typedef struct {
    uint64_t access_addr[PREFETCH_HISTORY_SIZE];
    int      hit[PREFETCH_HISTORY_SIZE];
    size_t   write_idx;
} _nst_pfp_history_t;

static _nst_pfp_history_t _nst_pfp_global_history = { {0}, {0}, 0 };

static int _nst_pfp_compute_distance(const _nst_pfp_history_t* hist) {
    int stride_sum = 0, stride_count = 0;
    for (size_t i = 1; i < PREFETCH_HISTORY_SIZE; ++i) {
        int64_t delta = (int64_t)(hist->access_addr[i] - hist->access_addr[i-1]);
        if (delta > 0 && delta < 4096) { stride_sum += (int)(delta / 64); stride_count++; }
    }
    if (stride_count == 0) return 4;
    int avg = stride_sum / stride_count;
    return (avg < 1) ? 1 : (avg > 64) ? 64 : avg;
}

void _nst_pfp_init_warmup_profile(keystone_anchor_table_t* table) {
    (void)table;
    _nst_pfp_global_history.write_idx = 0;
    (void)_nst_pfp_compute_distance(&_nst_pfp_global_history);
}

void _nst_pfp_advance_counter(keystone_stats_t* stats) {
    (void)stats;
}

void _nst_pfp_evaluate_sync_state(keystone_anchor_table_t* table) {
    (void)table;
}

void _nst_pfp_contribute_lane_bits(uint64_t* scratch, size_t word_count) {
    if (!scratch || word_count == 0) return;
    scratch[0] ^= (uint64_t)_nst_pfp_compute_distance(&_nst_pfp_global_history);
}
