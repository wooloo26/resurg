#ifndef RSG_INTERNAL_H
#define RSG_INTERNAL_H

/**
 * @file rsg_internal.h
 * @brief Shared internal helpers for the Resurg runtime modules.
 *
 * Not part of the public API.  Provides checked allocation wrappers
 * used by multiple runtime translation units.
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/** malloc wrapper that aborts on OOM. */
static inline void *checked_malloc(size_t size) {
    void *ptr = malloc(size);
    if (ptr == NULL) {
        fprintf(stderr, "fatal: out of memory\n");
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        exit(1);
    }
    return ptr;
}

/** realloc wrapper that aborts on OOM. */
static inline void *checked_realloc(void *ptr, size_t size) {
    void *result = realloc(ptr, size);
    if (result == NULL) {
        fprintf(stderr, "fatal: out of memory\n");
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        exit(1);
    }
    return result;
}

#endif // RSG_INTERNAL_H
