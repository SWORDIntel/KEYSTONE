/* NST Platform Hints - local NUMA placement hints */

#include "nst_platform_hints.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#define MAX_NUMA_NODES 8

static int _nst_hints_numa_distance[MAX_NUMA_NODES][MAX_NUMA_NODES];
static int _nst_hints_node_count = 1;

static void _nst_hints_build_numa_matrix(void) {
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 1) ncpu = 1;
    int nodes = (ncpu > 64) ? MAX_NUMA_NODES : (int)((ncpu + 7) / 8);
    if (nodes < 1) nodes = 1;
    _nst_hints_node_count = nodes;

    for (int i = 0; i < nodes; ++i) {
        for (int j = 0; j < nodes; ++j) {
            _nst_hints_numa_distance[i][j] = (i == j) ? 10 : 20 + abs(i-j)*5;
        }
    }
}

void _nst_hints_extract_domain_context(_nst_numa_placement_hint_t* hint) {
    if (!hint) return;
    _nst_hints_build_numa_matrix();

    hint->preferred_node = 0;
    hint->domain_confidence = 0;
    hint->domain_label[0] = '\0';
    hint->reachability_flags = 0;
    hint->latency_estimate = (double)_nst_hints_numa_distance[0][0];

    if (_nst_hints_node_count > 1) {
        int best_node = 0;
        int best_distance = _nst_hints_numa_distance[0][0];
        for (int i = 1; i < _nst_hints_node_count; ++i) {
            if (_nst_hints_numa_distance[0][i] < best_distance) {
                best_distance = _nst_hints_numa_distance[0][i];
                best_node = i;
            }
        }
        hint->preferred_node = best_node;
        hint->latency_estimate = (double)best_distance;
    }
}

int _nst_hints_score_environment(const _nst_numa_placement_hint_t* hint) {
    if (!hint) return 0;
    return hint->latency_estimate <= 10.0 ? 2 : 1;
}

void _nst_hints_contribute_lane_bits(uint64_t* scratch, size_t word_count) {
    if (!scratch || word_count == 0) return;
    scratch[0] ^= (uint64_t)_nst_hints_node_count;
}
