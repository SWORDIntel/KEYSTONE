#include "keystone_cuda.h"
#include <cuda_runtime.h>
#include <stdio.h>
#include <mutex>

// Use __ldg to route reads through the read-only texture cache for high warp divergence efficiency
#if __CUDA_ARCH__ >= 350
#define LDG(ptr) __ldg(ptr)
#else
#define LDG(ptr) (*(ptr))
#endif

// Global device cache to prevent catastrophic PCIe overhead on repeated batch calls
static std::mutex g_cache_mutex;
static const int64_t* g_cached_h_arr = NULL;
static int64_t* g_cached_d_arr = NULL;
static size_t g_cached_n = 0;

__global__ void keystone_search_kernel_optimized(
    const int64_t* __restrict__ arr,
    size_t n,
    keystone_batch_item_t* __restrict__ items,
    size_t num_items,
    unsigned long long* __restrict__ d_success_count)
{
    size_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= num_items) return;

    int64_t key = items[idx].key;
    size_t found_idx = KEYSTONE_NOT_FOUND;

    // Quick bounds check using texture cache
    if (n > 0 && key >= LDG(&arr[0]) && key <= LDG(&arr[n - 1])) {
        // Branchless lower-bound binary search
        // This minimizes thread divergence within a warp when threads are searching for disparate keys
        size_t lo = 0;
        size_t len = n;
        
        while (len > 1) {
            size_t half = len / 2;
            size_t mid = lo + half - 1;
            int64_t mid_val = LDG(&arr[mid]);
            
            // Branchless advance
            lo = (mid_val < key) ? (lo + half) : lo;
            len -= half;
        }
        
        if (LDG(&arr[lo]) == key) {
            found_idx = lo;
        }
    }

    items[idx].result = found_idx;
    if (found_idx != KEYSTONE_NOT_FOUND) {
        atomicAdd(d_success_count, 1ULL);
    }
}

extern "C" size_t keystone_search_batch_cuda(
    const int64_t* arr,
    size_t n,
    keystone_batch_item_t* items,
    size_t num_items)
{
    if (n == 0 || num_items == 0) return 0;

    int64_t* d_arr = NULL;
    
    // Lock the cache to safely reuse device memory across repeated batch queries
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        if (g_cached_h_arr == arr && g_cached_n == n) {
            d_arr = g_cached_d_arr;
        } else {
            if (g_cached_d_arr) {
                cudaFree(g_cached_d_arr);
            }
            if (cudaMalloc((void**)&g_cached_d_arr, n * sizeof(int64_t)) == cudaSuccess) {
                cudaMemcpy(g_cached_d_arr, arr, n * sizeof(int64_t), cudaMemcpyHostToDevice);
                g_cached_h_arr = arr;
                g_cached_n = n;
                d_arr = g_cached_d_arr;
            } else {
                g_cached_h_arr = NULL;
                g_cached_n = 0;
                g_cached_d_arr = NULL;
                return 0; // OOM
            }
        }
    }

    keystone_batch_item_t* d_items = NULL;
    unsigned long long* d_success_count = NULL;

    if (cudaMalloc((void**)&d_items, num_items * sizeof(keystone_batch_item_t)) != cudaSuccess) return 0;
    if (cudaMalloc((void**)&d_success_count, sizeof(unsigned long long)) != cudaSuccess) {
        cudaFree(d_items);
        return 0;
    }

    // Use asynchronous stream for parallel dispatch
    cudaStream_t stream;
    cudaStreamCreate(&stream);

    // Upload items to device asynchronously
    cudaMemcpyAsync(d_items, items, num_items * sizeof(keystone_batch_item_t), cudaMemcpyHostToDevice, stream);
    cudaMemsetAsync(d_success_count, 0, sizeof(unsigned long long), stream);

    // Compute optimal thread blocks
    int threads_per_block = 256;
    int blocks = (num_items + threads_per_block - 1) / threads_per_block;

    // Launch optimized kernel
    keystone_search_kernel_optimized<<<blocks, threads_per_block, 0, stream>>>(
        d_arr, n, d_items, num_items, d_success_count);

    // Download items back asynchronously
    cudaMemcpyAsync(items, d_items, num_items * sizeof(keystone_batch_item_t), cudaMemcpyDeviceToHost, stream);

    unsigned long long h_success_count = 0;
    cudaMemcpyAsync(&h_success_count, d_success_count, sizeof(unsigned long long), cudaMemcpyDeviceToHost, stream);

    // Sync stream to ensure all operations finish before returning
    cudaStreamSynchronize(stream);
    cudaStreamDestroy(stream);

    cudaFree(d_items);
    cudaFree(d_success_count);

    return (size_t)h_success_count;
}
