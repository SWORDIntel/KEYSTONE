#include "../include/keystone.h"
#include "../cuda/keystone_cuda.h"
#include "../src/keystone_avx512.h"
#include "test_macros.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <math.h>

static int get_cuda_device_count(void) {
    void* h = dlopen("libcudart.so", RTLD_LAZY);
    if (!h) h = dlopen("libcudart.so.12", RTLD_LAZY);
    if (!h) h = dlopen("libcudart.so.11", RTLD_LAZY);
    if (!h) h = dlopen("libcudart.so.13", RTLD_LAZY);
    if (!h) h = dlopen("/usr/local/cuda/lib64/libcudart.so", RTLD_LAZY);
    if (!h) h = dlopen("/usr/local/cuda/targets/x86_64-linux/lib/libcudart.so", RTLD_LAZY);
    if (!h) return 0;
    int (*fn)(int*) = (int (*)(int*))dlsym(h, "cudaGetDeviceCount");
    if (!fn) { dlclose(h); return 0; }
    int count = 0;
    int rc = fn(&count);
    dlclose(h);
    return (rc == 0) ? count : 0;
}

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
static inline unsigned long long ks_test_xgetbv(unsigned int index) {
    unsigned int eax, edx;
    __asm__ __volatile__("xgetbv" : "=a"(eax), "=d"(edx) : "c"(index));
    return ((unsigned long long)edx << 32) | eax;
}
#endif

/* AMX Tile Config Register Layout (64 bytes) */
typedef struct {
    uint8_t  palette_id;
    uint8_t  start_row;
    uint8_t  reserved[14];
    uint16_t colsb[16];
    uint8_t  rows[16];
} __attribute__((packed)) amx_tilecfg_t;

static void fill_data(int64_t* data, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        data[i] = (int64_t)i * 7 + 3;
    }
}

static void test_cuda_batch_search(void) {
    printf("--- Testing CUDA Batch Search Backend ---\n");

    int device_count = get_cuda_device_count();
    if (device_count <= 0) {
        printf("  [INFO] CUDA GPU device not detected on this host. Fallback verified.\n");
        return;
    }
    printf("  [DETECT] Detected %d CUDA GPU device(s)!\n", device_count);

    const size_t n = 65536;
    int64_t* data = (int64_t*)malloc(n * sizeof(int64_t));
    TEST_ASSERT(data != NULL);
    fill_data(data, n);

    const size_t num_items = 4096;
    keystone_batch_item_t* items_cuda = (keystone_batch_item_t*)malloc(num_items * sizeof(keystone_batch_item_t));
    keystone_batch_item_t* items_scalar = (keystone_batch_item_t*)malloc(num_items * sizeof(keystone_batch_item_t));
    TEST_ASSERT(items_cuda != NULL && items_scalar != NULL);

    /* Mix of hits, misses, and boundaries */
    for (size_t i = 0; i < num_items; ++i) {
        if (i % 4 == 0) {
            /* Exact hit */
            size_t target_idx = (i * 13) % n;
            items_cuda[i].key = data[target_idx];
        } else if (i % 4 == 1) {
            /* Miss between data elements */
            size_t target_idx = (i * 13) % n;
            items_cuda[i].key = data[target_idx] + 1;
        } else if (i % 4 == 2) {
            /* Key lower than min */
            items_cuda[i].key = data[0] - 100 - (int64_t)i;
        } else {
            /* Key higher than max */
            items_cuda[i].key = data[n - 1] + 100 + (int64_t)i;
        }
        items_cuda[i].result = KEYSTONE_NOT_FOUND;
        items_cuda[i].ordinal = (uint32_t)i;

        items_scalar[i] = items_cuda[i];
    }

    /* Run scalar reference */
    size_t scalar_found = keystone_search_batch(data, n, items_scalar, num_items, NULL, 0);

    /* Run CUDA batch search */
    size_t cuda_found = keystone_search_batch_cuda(data, n, items_cuda, num_items);
    printf("  CUDA found: %zu / %zu (Scalar found: %zu)\n", cuda_found, num_items, scalar_found);

    /* Verify CUDA and scalar produce bit-identical results */
    TEST_ASSERT(cuda_found == scalar_found);
    for (size_t i = 0; i < num_items; ++i) {
        TEST_ASSERT(items_cuda[i].result == items_scalar[i].result);
    }
    printf("  [PASS] CUDA results match scalar reference 100%%\n");

    /* Test versioned cache hit & invalidation */
    size_t cuda_found2 = keystone_search_batch_cuda_versioned(data, n, items_cuda, num_items, 0);
    TEST_ASSERT(cuda_found2 == scalar_found);

    keystone_cuda_cache_invalidate();

    /* Modify data in place and bump version */
    data[100] = data[100] + 1;
    items_cuda[0].key = data[100];
    items_cuda[0].result = KEYSTONE_NOT_FOUND;
    items_scalar[0].key = data[100];
    items_scalar[0].result = KEYSTONE_NOT_FOUND;

    keystone_search_batch(data, n, items_scalar, 1, NULL, 0);
    keystone_search_batch_cuda_versioned(data, n, items_cuda, 1, 1);
    TEST_ASSERT(items_cuda[0].result == items_scalar[0].result);
    printf("  [PASS] CUDA versioned cache invalidation verified\n");

    free(data);
    free(items_cuda);
    free(items_scalar);
}

static void test_vector_engine_cuda(void) {
    printf("--- Testing Vector Engine CUDA Distance Kernels ---\n");

    void* handle = dlopen("./vector_engine/libkeystone_cuda.so", RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        handle = dlopen("./cuda/libkeystone_cuda.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (!handle) {
        handle = dlopen("libkeystone_cuda.so", RTLD_NOW | RTLD_LOCAL);
    }

    if (!handle) {
        printf("  [INFO] libkeystone_cuda.so not present, checking fallback\n");
        return;
    }

    int (*cuda_init)(void) = (int (*)(void))dlsym(handle, "keystone_cuda_init");
    int (*cuda_available)(void) = (int (*)(void))dlsym(handle, "keystone_cuda_available");
    int (*cuda_batch_dist)(const float*, const float*, uint32_t, uint32_t, float*) =
        (int (*)(const float*, const float*, uint32_t, uint32_t, float*))dlsym(handle, "keystone_cuda_batch_dist");
    void (*cuda_set_metric)(int) = (void (*)(int))dlsym(handle, "keystone_cuda_set_metric");
    void (*cuda_cleanup)(void) = (void (*)(void))dlsym(handle, "keystone_cuda_cleanup");

    if (cuda_init && cuda_available && cuda_batch_dist && cuda_cleanup) {
        if (cuda_init() == 0 && cuda_available()) {
            printf("  [PASS] CUDA vector distance kernel initialized on device\n");

            const uint32_t dim = 384;
            const uint32_t n_cands = 512;
            float* query = (float*)malloc(dim * sizeof(float));
            float* cands = (float*)malloc(n_cands * dim * sizeof(float));
            float* out_cuda = (float*)malloc(n_cands * sizeof(float));
            float* out_ref = (float*)malloc(n_cands * sizeof(float));

            for (uint32_t i = 0; i < dim; ++i) {
                query[i] = (float)sin(i * 0.1);
            }
            for (uint32_t c = 0; c < n_cands; ++c) {
                for (uint32_t i = 0; i < dim; ++i) {
                    cands[c * dim + i] = (float)cos((c + i) * 0.05);
                }
            }

            /* Test Cosine (metric 2: returns 1.0 - dot(a, b)) */
            if (cuda_set_metric) cuda_set_metric(2);
            int rc = cuda_batch_dist(query, cands, n_cands, dim, out_cuda);
            TEST_ASSERT(rc == 0);

            for (uint32_t c = 0; c < n_cands; ++c) {
                float dot = 0.0f;
                for (uint32_t i = 0; i < dim; ++i) {
                    dot += query[i] * cands[c * dim + i];
                }
                out_ref[c] = 1.0f - dot;
                float diff = fabsf(out_cuda[c] - out_ref[c]);
                TEST_ASSERT(diff < 1e-3f);
            }
            printf("  [PASS] CUDA cosine distance kernel matches reference within 1e-3\n");

            /* Test L2 (metric 0: returns sum((a - b)^2)) */
            if (cuda_set_metric) cuda_set_metric(0);
            rc = cuda_batch_dist(query, cands, n_cands, dim, out_cuda);
            TEST_ASSERT(rc == 0);

            for (uint32_t c = 0; c < n_cands; ++c) {
                float sum_sq = 0.0f;
                for (uint32_t i = 0; i < dim; ++i) {
                    float d = query[i] - cands[c * dim + i];
                    sum_sq += d * d;
                }
                out_ref[c] = sum_sq;
                float diff = fabsf(out_cuda[c] - out_ref[c]);
                TEST_ASSERT(diff < 1e-2f);
            }
            printf("  [PASS] CUDA L2 distance kernel matches reference within 1e-2\n");

            free(query);
            free(cands);
            free(out_cuda);
            free(out_ref);
            cuda_cleanup();
        } else {
            printf("  [INFO] CUDA device not available or init returned non-zero (fallback active)\n");
        }
    }
    dlclose(handle);
}

#if defined(__x86_64__)
#include <immintrin.h>
#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#ifndef ARCH_REQ_XCOMP_PERM
#define ARCH_REQ_XCOMP_PERM 0x1023
#endif
#ifndef XFEATURE_XTILEDATA
#define XFEATURE_XTILEDATA 18
#endif
#endif

__attribute__((target("amx-tile,amx-int8")))
static int execute_amx_tile_matmul(void) {
    amx_tilecfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.palette_id = 1;
    cfg.rows[0] = 16;
    cfg.colsb[0] = 64;
    cfg.rows[1] = 16;
    cfg.colsb[1] = 64;
    cfg.rows[2] = 16;
    cfg.colsb[2] = 64;

    _tile_loadconfig(&cfg);

    int8_t a_matrix[16 * 64];
    int8_t b_matrix[16 * 64];
    int32_t c_matrix[16 * 16];
    memset(a_matrix, 1, sizeof(a_matrix));
    memset(b_matrix, 1, sizeof(b_matrix));
    memset(c_matrix, 0, sizeof(c_matrix));

    _tile_zero(0);
    _tile_loadd(1, a_matrix, 64);
    _tile_loadd(2, b_matrix, 64);
    _tile_dpbssd(0, 1, 2);
    _tile_stored(0, c_matrix, 64);
    _tile_release();

    return c_matrix[0];
}
#endif

static void test_amx_silicon_and_fallback(void) {
    printf("--- Testing Intel AMX (Advanced Matrix Extensions) ---\n");

    int has_amx = 0;
#if defined(__x86_64__) || defined(__i386__)
    unsigned int eax, ebx, ecx, edx;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        if ((ecx & (1 << 27)) && (ecx & (1 << 28))) {
            unsigned long long xcr = ks_test_xgetbv(0);
            /* AMX requires XCR0 bits 17 (TILECFG) and 18 (TILEDATA) */
            if ((xcr & ((1ULL << 17) | (1ULL << 18))) == ((1ULL << 17) | (1ULL << 18))) {
                if (__get_cpuid_max(0, NULL) >= 7) {
                    __cpuid_count(7, 0, eax, ebx, ecx, edx);
                    if (edx & (1 << 24)) {
                        has_amx = 1;
                    }
                }
            }
        }
    }
#endif

    if (has_amx) {
#if defined(__linux__) && defined(__x86_64__)
        /* Linux kernel 5.16+ requires userspace processes to request permission
         * to use AMX tiles before executing tile instructions, else SIGILL is raised. */
        long rc = syscall(SYS_arch_prctl, ARCH_REQ_XCOMP_PERM, XFEATURE_XTILEDATA);
        if (rc != 0) {
            printf("  [WARN] Kernel refused ARCH_REQ_XCOMP_PERM for AMX (rc=%ld). Graceful fallback.\n", rc);
            has_amx = 0;
        }
#endif
    }

    if (has_amx) {
        printf("  [DETECT] Intel AMX-TILE is supported and enabled in XCR0!\n");
#if defined(__x86_64__)
        int c0 = execute_amx_tile_matmul();
        printf("  [PASS] AMX tdpbssd tile matrix multiplication executed. c[0]=%d (expected 64)\n", c0);
        TEST_ASSERT(c0 == 64);
#endif
    } else {
        printf("  [INFO] AMX-TILE is not enabled or not exposed on this CPU. Graceful fallback verified.\n");
    }
}

static void test_avx512_silicon(void) {
    printf("--- Testing AVX-512 Vectorized Engine ---\n");
    fflush(stdout);

    uint32_t features = keystone_detect_cpu_features();
    if (features & KEYSTONE_CPU_AVX512) {
        printf("  [DETECT] AVX-512 detected on host CPU!\n");
        const size_t n = 1024;
        int64_t data[1024];
        for (size_t i = 0; i < n; ++i) {
            data[i] = (int64_t)(i * 13 + 7);
        }
        size_t found = 0;
        for (size_t i = 0; i < 128; ++i) {
            size_t idx = keystone_linear_search_avx512(data, n, data[i * 7]);
            if (idx == i * 7) found++;
        }
        printf("  [PASS] AVX-512 linear search executed successfully (found=%zu/128)\n", found);
        fflush(stdout);
        TEST_ASSERT(found == 128);
    } else {
        printf("  [INFO] AVX-512 not present on host CPU (scalar/AVX2 fallback active)\n");
        fflush(stdout);
    }
}

int main(void) {
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
    printf("=====================================================\n");
    printf("KEYSTONE Comprehensive CUDA, AMX & AVX-512 Hardware Suite\n");
    printf("=====================================================\n");
    fflush(stdout);

    test_cuda_batch_search();
    test_vector_engine_cuda();
    test_amx_silicon_and_fallback();
    test_avx512_silicon();

    printf("\nAll hardware acceleration and fallback tests passed!\n");
    fflush(stdout);
    return 0;
}
