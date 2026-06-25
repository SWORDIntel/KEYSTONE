#ifndef KEYSTONE_CUDA_H
#define KEYSTONE_CUDA_H

#include "../include/keystone.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Perform a batch search using CUDA.
 * 
 * This is a standalone proof-of-concept backend that allocates
 * memory on the GPU, copies the array and batch items, performs
 * binary search in parallel, and copies the results back.
 * 
 * @param arr Pointer to the sorted array.
 * @param n Size of the array.
 * @param items Array of batch items to search for.
 * @param num_items Number of items in the batch.
 * @return Number of successful searches.
 */
size_t keystone_search_batch_cuda(
    const int64_t* arr,
    size_t n,
    keystone_batch_item_t* items,
    size_t num_items
);

#ifdef __cplusplus
}
#endif

#endif // KEYSTONE_CUDA_H
