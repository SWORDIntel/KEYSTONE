#include <cuda_runtime.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include "keystone_cuda.h"

// Use __ldg to route reads through the read-only texture cache for high warp divergence efficiency
#if __CUDA_ARCH__ >= 350
#define LDG(ptr) __ldg(ptr)
#else
#define LDG(ptr) (*(ptr))
#endif

// ============================================================================
// Thread-safe device-array cache with reader leases
// ============================================================================
//
// The previous implementation released the spinlock *before* launching the
// kernel that uses d_arr.  A concurrent thread could then evict and cudaFree
// the buffer while the first thread's kernel was still referencing it — a
// GPU use-after-free.
//
// The new design uses a generation + reader-count protocol:
//
//   Cache hit:
//     1. Acquire mutex.
//     2. Verify identity (h_arr + n + version).
//     3. Increment readers, increment generation (to pin the slot).
//     4. Release mutex.
//     5. Launch kernel using d_arr.
//     6. cudaStreamSynchronize.
//     7. Decrement readers.
//
//   Cache miss / eviction:
//     1. Acquire mutex.
//     2. Wait for readers == 0 (other threads may be using the old buffer).
//     3. cudaStreamSynchronize the slot's stream.
//     4. cudaFree old d_arr, allocate new one, copy.
//     5. Update identity + generation.
//     6. Increment readers (for the current caller).
//     7. Release mutex.
//
// The mutex serializes install/evict but NOT kernel execution, so concurrent
// cache hits run in parallel.  Only the brief critical section is serialized.
//
// A caller-supplied dataset version (default 0) lets callers that mutate the
// host array in-place signal that the cached device copy is stale.
// ============================================================================

// Slot states
#define KSLOT_FREE      0
#define KSLOT_WRITING   1
#define KSLOT_VALID     2

typedef struct {
    // Identity (protected by cache_mutex)
    const int64_t* h_arr;
    int64_t* d_arr;
    size_t n;
    uint64_t version;       // caller-supplied dataset generation

    // Reader lease tracking (protected by cache_mutex)
    int state;              // KSLOT_*
    int readers;            // number of threads currently using this slot
    uint64_t generation;    // bumped on every install, used to detect eviction
    cudaStream_t stream;    // per-slot stream for synchronization
    int has_stream;
} keystone_cuda_cache_slot_t;

static keystone_cuda_cache_slot_t g_slot = {0};
static pthread_mutex_t g_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

// ----------------------------------------------------------------------------
// Kernels
// ----------------------------------------------------------------------------

__global__ void keystone_search_kernel_scalar(
    const int64_t* __restrict__ arr,
    size_t n,
    keystone_batch_item_t* __restrict__ items,
    size_t num_items,
    unsigned long long* __restrict__ d_success_count)
{
    // Scalar Branchless Binary Search (Optimized for older GPUs / Pascal / low SM count)
    size_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= num_items) return;

    int64_t key = items[idx].key;
    size_t found_idx = KEYSTONE_NOT_FOUND;

    // Quick bounds check using texture cache
    if (n > 0 && key >= LDG(&arr[0]) && key <= LDG(&arr[n - 1])) {
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

__global__ void keystone_search_kernel_warp_cooperative(
    const int64_t* __restrict__ arr,
    size_t n,
    keystone_batch_item_t* __restrict__ items,
    size_t num_items,
    unsigned long long* __restrict__ d_success_count)
{
    // Warp-Cooperative 32-ary Search (Optimized for H200 / Hopper)
    unsigned int tid = threadIdx.x;
    unsigned int lane_id = tid % 32;
    unsigned int warp_id = (blockIdx.x * blockDim.x + tid) / 32;

    if (warp_id >= num_items) return;

    int64_t key = items[warp_id].key;
    size_t found_idx = KEYSTONE_NOT_FOUND;

    if (n > 0) {
        int64_t bound_min = (lane_id == 0) ? LDG(&arr[0]) : 0;
        int64_t bound_max = (lane_id == 31) ? LDG(&arr[n - 1]) : 0;

        bound_min = __shfl_sync(0xFFFFFFFF, bound_min, 0);
        bound_max = __shfl_sync(0xFFFFFFFF, bound_max, 31);

        if (key >= bound_min && key <= bound_max) {
            size_t lo = 0;
            size_t hi = n;

            while (hi - lo > 32) {
                size_t step = (hi - lo) / 32;
                size_t probe_idx = lo + lane_id * step;

                int64_t probe_val = LDG(&arr[probe_idx]);

                unsigned int mask = __ballot_sync(0xFFFFFFFF, probe_val <= key);

                int highest_lane = 31 - __clz(mask);

                lo = lo + highest_lane * step;
                hi = (highest_lane == 31) ? hi : (lo + step);
            }

            size_t len = hi - lo;
            size_t probe_idx = lo + lane_id;

            int is_match = 0;
            if (lane_id < len && LDG(&arr[probe_idx]) == key) {
                is_match = 1;
            }

            unsigned int match_mask = __ballot_sync(0xFFFFFFFF, is_match);
            if (match_mask != 0) {
                int match_lane = __ffs(match_mask) - 1;
                found_idx = lo + match_lane;
            }
        }
    }

    if (lane_id == 0) {
        items[warp_id].result = found_idx;
        if (found_idx != KEYSTONE_NOT_FOUND) {
            atomicAdd(d_success_count, 1ULL);
        }
    }
}

// ----------------------------------------------------------------------------
// Internal helpers (called with mutex held)
// ----------------------------------------------------------------------------

static void slot_destroy_stream(keystone_cuda_cache_slot_t* s) {
    if (s->has_stream) {
        cudaStreamSynchronize(s->stream);
        cudaStreamDestroy(s->stream);
        s->has_stream = 0;
    }
}

static int slot_ensure_stream(keystone_cuda_cache_slot_t* s) {
    if (s->has_stream) return 0;
    if (cudaStreamCreate(&s->stream) != cudaSuccess) return -1;
    s->has_stream = 1;
    return 0;
}

static void slot_evict(keystone_cuda_cache_slot_t* s) {
    // Wait for any outstanding readers before freeing.
    while (s->readers > 0) {
        pthread_mutex_unlock(&g_cache_mutex);
        // Brief spin-wait outside the mutex to allow readers to finish.
        for (volatile int spin = 0; spin < 1000; ++spin) { }
        pthread_mutex_lock(&g_cache_mutex);
    }
    slot_destroy_stream(s);
    if (s->d_arr) {
        cudaFree(s->d_arr);
        s->d_arr = NULL;
    }
    s->h_arr = NULL;
    s->n = 0;
    s->version = 0;
    s->state = KSLOT_FREE;
}

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------

extern "C" size_t keystone_search_batch_cuda(
    const int64_t* arr,
    size_t n,
    keystone_batch_item_t* items,
    size_t num_items)
{
    // Default version = 0 (callers that mutate arr in-place should use
    // keystone_search_batch_cuda_versioned to bump the version).
    return keystone_search_batch_cuda_versioned(arr, n, items, num_items, 0);
}

extern "C" size_t keystone_search_batch_cuda_versioned(
    const int64_t* arr,
    size_t n,
    keystone_batch_item_t* items,
    size_t num_items,
    uint64_t dataset_version)
{
    if (n == 0 || num_items == 0) return 0;

    int64_t* d_arr = NULL;
    cudaStream_t slot_stream;

    // --- Acquire a reader lease on the cached device array ---
    pthread_mutex_lock(&g_cache_mutex);

    int cache_hit = (g_slot.state == KSLOT_VALID &&
                     g_slot.h_arr == arr &&
                     g_slot.n == n &&
                     g_slot.version == dataset_version &&
                     g_slot.d_arr != NULL);

    if (cache_hit) {
        // Pin the slot: increment readers so eviction can't free d_arr
        // while our kernel is in flight.
        g_slot.readers++;
        d_arr = g_slot.d_arr;
        slot_stream = g_slot.stream;
    } else {
        // Cache miss: evict the old slot (waits for any existing readers).
        if (g_slot.state == KSLOT_VALID || g_slot.d_arr) {
            slot_evict(&g_slot);
        }
        if (slot_ensure_stream(&g_slot) != 0) {
            pthread_mutex_unlock(&g_cache_mutex);
            return 0;  // OOM / stream creation failure
        }
        g_slot.state = KSLOT_WRITING;
        if (cudaMalloc((void**)&g_slot.d_arr, n * sizeof(int64_t)) != cudaSuccess) {
            g_slot.d_arr = NULL;
            g_slot.state = KSLOT_FREE;
            pthread_mutex_unlock(&g_cache_mutex);
            return 0;  // OOM
        }
        cudaMemcpy(g_slot.d_arr, arr, n * sizeof(int64_t), cudaMemcpyHostToDevice);
        g_slot.h_arr = arr;
        g_slot.n = n;
        g_slot.version = dataset_version;
        g_slot.generation++;
        g_slot.state = KSLOT_VALID;
        g_slot.readers = 1;  // lease for the current caller
        d_arr = g_slot.d_arr;
        slot_stream = g_slot.stream;
    }

    pthread_mutex_unlock(&g_cache_mutex);

    // --- Allocate per-call buffers (d_items, d_success_count) ---
    keystone_batch_item_t* d_items = NULL;
    unsigned long long* d_success_count = NULL;

    if (cudaMalloc((void**)&d_items, num_items * sizeof(keystone_batch_item_t)) != cudaSuccess) {
        pthread_mutex_lock(&g_cache_mutex);
        g_slot.readers--;
        pthread_mutex_unlock(&g_cache_mutex);
        return 0;
    }
    if (cudaMalloc((void**)&d_success_count, sizeof(unsigned long long)) != cudaSuccess) {
        cudaFree(d_items);
        pthread_mutex_lock(&g_cache_mutex);
        g_slot.readers--;
        pthread_mutex_unlock(&g_cache_mutex);
        return 0;
    }

    // Use a per-call stream for items so we don't block the slot stream
    // (which other threads may be using for their own kernels).
    cudaStream_t call_stream;
    cudaStreamCreate(&call_stream);

    cudaMemcpyAsync(d_items, items, num_items * sizeof(keystone_batch_item_t),
                    cudaMemcpyHostToDevice, call_stream);
    cudaMemsetAsync(d_success_count, 0, sizeof(unsigned long long), call_stream);

    // Adaptive Pathway Decision based on GPU architecture and capability
    cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, 0);

    bool use_warp_cooperative = (prop.major >= 6);

    if (use_warp_cooperative) {
        int threads_per_block = 256;
        int warps_per_block = threads_per_block / 32;
        int blocks = (num_items + warps_per_block - 1) / warps_per_block;

        keystone_search_kernel_warp_cooperative<<<blocks, threads_per_block, 0, call_stream>>>(
            d_arr, n, d_items, num_items, d_success_count);
    } else {
        int threads_per_block = 256;
        int blocks = (num_items + threads_per_block - 1) / threads_per_block;

        keystone_search_kernel_scalar<<<blocks, threads_per_block, 0, call_stream>>>(
            d_arr, n, d_items, num_items, d_success_count);
    }

    cudaMemcpyAsync(items, d_items, num_items * sizeof(keystone_batch_item_t),
                    cudaMemcpyDeviceToHost, call_stream);

    unsigned long long h_success_count = 0;
    cudaMemcpyAsync(&h_success_count, d_success_count, sizeof(unsigned long long),
                    cudaMemcpyDeviceToHost, call_stream);

    cudaStreamSynchronize(call_stream);
    cudaStreamDestroy(call_stream);

    cudaFree(d_items);
    cudaFree(d_success_count);

    // --- Release the reader lease ---
    pthread_mutex_lock(&g_cache_mutex);
    g_slot.readers--;
    pthread_mutex_unlock(&g_cache_mutex);

    return (size_t)h_success_count;
}

extern "C" void keystone_cuda_cache_invalidate(void) {
    pthread_mutex_lock(&g_cache_mutex);
    slot_evict(&g_slot);
    pthread_mutex_unlock(&g_cache_mutex);
}
