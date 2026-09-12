#ifndef KEYSTONE_SAFE_ALLOC_H
#define KEYSTONE_SAFE_ALLOC_H

/*
 * Overflow-safe allocation helpers.
 *
 * Every n * sizeof(T) and a + b that feeds malloc/calloc/realloc must be
 * checked.  These helpers return the product/sum via an out-parameter and
 * return false on overflow, so the caller can fail gracefully.
 *
 * Usage pattern:
 *   size_t bytes;
 *   if (!checked_mul_size(n, sizeof(T), &bytes)) return NULL;
 *   T *p = malloc(bytes);
 *   if (!p) return NULL;
 */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

static inline bool checked_mul_size(size_t a, size_t b, size_t *out) {
    if (!out) return false;
    if (a != 0u && b > SIZE_MAX / a) return false;
    *out = a * b;
    return true;
}

static inline bool checked_add_size(size_t a, size_t b, size_t *out) {
    if (!out) return false;
    if (b > SIZE_MAX - a) return false;
    *out = a + b;
    return true;
}

/*
 * Convenience: check n > SIZE_MAX / elem_size without needing an out-param.
 * Returns true if the product n * elem_size is representable in size_t.
 */
static inline bool safe_mul_check(size_t n, size_t elem_size) {
    return (n == 0u) || (elem_size <= SIZE_MAX / n);
}

#endif /* KEYSTONE_SAFE_ALLOC_H */
