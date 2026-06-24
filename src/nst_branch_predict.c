/* NST Branch-Prediction Warm-Up Tracker & BTB Seed Generator */

#include "nst_branch_predict.h"
#include <stdint.h>

#define BTB_HISTORY_SIZE 256

static uint8_t _nst_bp_history[BTB_HISTORY_SIZE] = {0};
static size_t   _nst_bp_history_idx = 0;

static uint32_t _nst_bp_hash(uint64_t addr) {
    uint32_t h = (uint32_t)(addr ^ (addr >> 33));
    h = (h * 0x9E3779B9U) >> 16;
    return h % BTB_HISTORY_SIZE;
}

float _nst_bp_warmup_predictor(uint64_t branch_addr) {
    uint32_t idx = _nst_bp_hash(branch_addr);
    uint8_t taken = _nst_bp_history[idx];
    _nst_bp_history[_nst_bp_history_idx % BTB_HISTORY_SIZE] = (taken > 128) ? 255 : 0;
    _nst_bp_history_idx++;
    return (taken > 128) ? 0.95f : 0.05f;
}

void _nst_bp_contribute_lane_bits(uint64_t* scratch, size_t word_count) {
    if (!scratch || word_count == 0) return;
    scratch[0] ^= (uint64_t)(_nst_bp_history_idx & 0xFFFFu);
}
