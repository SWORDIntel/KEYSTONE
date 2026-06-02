/* Lightweight test macros that print the failing expression, file, and line. */
#ifndef TEST_MACROS_H
#define TEST_MACROS_H

#include <stdio.h>
#include <stdlib.h>

#define TEST_ASSERT(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "ASSERTION FAILED: %s at %s:%d in %s\n", \
                    #cond, __FILE__, __LINE__, __func__); \
            abort(); \
        } \
    } while (0)

#endif /* TEST_MACROS_H */
