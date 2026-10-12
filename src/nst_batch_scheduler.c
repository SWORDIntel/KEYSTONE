/* NST Batch Scheduler - local latency feedback loop */

#include "nst_batch_scheduler.h"
#include <stddef.h>

#define LATENCY_RING_SIZE 128
typedef struct {
    double latency_ns[LATENCY_RING_SIZE];
    size_t write_idx;
} _nst_sched_ring_t;

static _nst_sched_ring_t _nst_sched_ring = {{0}, 0};

static void _nst_sched_record_sample(double ns) {
    _nst_sched_ring.latency_ns[_nst_sched_ring.write_idx % LATENCY_RING_SIZE] = ns;
    _nst_sched_ring.write_idx++;
}

void _nst_sched_emit_latency_sample(const char* payload_json) {
    size_t payload_len = 0;
    if (payload_json) {
        while (payload_json[payload_len] != '\0' && payload_len < 4096) {
            payload_len++;
        }
    }
    _nst_sched_record_sample((double)payload_len);
}
