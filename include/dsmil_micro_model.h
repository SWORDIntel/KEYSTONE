#ifndef DSMIL_MICRO_MODEL_H
#define DSMIL_MICRO_MODEL_H

#include "dsmil_model_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSMIL_MODEL_INPUT_DIM    260  /* 256 trigram + 4 char-ratio features */
#define DSMIL_MODEL_HIDDEN_DIM   64
#define DSMIL_MODEL_NUM_CLASSES  6

/* Minimum softmax confidence to return a class label.
 * Anything below this is returned as CLASS_UNKNOWN — no hallucination.
 * For 6 classes, random chance = 0.167. 0.30 is 1.8x above chance. */
#define DSMIL_CONFIDENCE_THRESHOLD 0.30f

typedef enum {
    CLASS_GENERIC    = 0,  /* General professional — no strong signal     */
    CLASS_FINANCIAL  = 1,  /* Finance / banking / accounting / insurance  */
    CLASS_CORPORATE  = 2,  /* C-suite / director / VP / owner             */
    CLASS_GOVERNMENT = 3,  /* Government / military / public admin        */
    CLASS_HEALTHCARE = 4,  /* Medical / pharma / biotech / HIPAA-scope    */
    CLASS_TECHNOLOGY = 5,  /* IT / infosec / engineering / semiconductor  */
    CLASS_UNKNOWN    = 99  /* Low confidence or no job data — do not guess */
} dsmil_classification_t;

/**
 * @brief Initialize the micro-model (weights are statically compiled in).
 */
void dsmil_micro_model_init(void);

/**
 * @brief Runs inference on a context window extracted from a dirty log.
 *
 * If the maximum softmax score is below DSMIL_CONFIDENCE_THRESHOLD,
 * CLASS_UNKNOWN is returned regardless of the argmax — no hallucination.
 *
 * @param ctx        Context window from dsmil_extract_model_context().
 * @param out_scores Array of DSMIL_MODEL_NUM_CLASSES floats (softmax output).
 * @return           Classification label, or CLASS_UNKNOWN if uncertain.
 */
dsmil_classification_t dsmil_micro_model_infer(
    const dsmil_model_context_t* ctx,
    float out_scores[DSMIL_MODEL_NUM_CLASSES]
);

#ifdef __cplusplus
}
#endif
#endif
