#ifndef DSMIL_MICRO_MODEL_H
#define DSMIL_MICRO_MODEL_H

#include "dsmil_model_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSMIL_MODEL_INPUT_DIM 256
#define DSMIL_MODEL_HIDDEN_DIM 64
#define DSMIL_MODEL_NUM_CLASSES 3

typedef enum {
    CLASS_GENERIC = 0,
    CLASS_FINANCIAL = 1,
    CLASS_CORPORATE = 2
} dsmil_classification_t;

/**
 * @brief Initialize the micro-model (loads weights into memory).
 */
void dsmil_micro_model_init(void);

/**
 * @brief Runs inference on a perfectly extracted context window.
 * 
 * @param ctx The context window extracted from the archive.
 * @param out_scores Array of 3 floats to hold the softmax output.
 * @return The highest-scoring classification category.
 */
dsmil_classification_t dsmil_micro_model_infer(const dsmil_model_context_t* ctx, float out_scores[DSMIL_MODEL_NUM_CLASSES]);

#ifdef __cplusplus
}
#endif
#endif
