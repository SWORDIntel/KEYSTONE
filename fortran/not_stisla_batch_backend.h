#ifndef NOT_STISLA_BATCH_BACKEND_H
#define NOT_STISLA_BATCH_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void not_stisla_batch_search_i64(
    const int64_t* data,
    size_t n,
    const int64_t* keys,
    size_t key_count,
    int64_t* out_indices
);

#ifdef __cplusplus
}
#endif

#endif /* NOT_STISLA_BATCH_BACKEND_H */
