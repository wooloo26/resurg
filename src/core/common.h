#ifndef RSG_COMMON_H
#define RSG_COMMON_H

#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>

/**
 * @file common.h
 * @brief Umbrella header: arena, hash table, stretchy bufs, diagnostics.
 */

#include "arena.h"
#include "buf.h"
#include "hash_table.h"

/** A file:line:column triple attached to tokens and AST nodes. */
typedef struct {
    const char *file;
    int32_t line;
    int32_t column;
} SrcLoc;

/** Checked malloc - aborts on OOM. */
void *rsg_malloc(size_t size);
/** Checked calloc - aborts on OOM. */
void *rsg_calloc(size_t count, size_t size);
/** Checked realloc - aborts on OOM. */
void *rsg_realloc(void *ptr, size_t size);

/**
 * Emit "file:line:col: err: ..." to stderr.
 */
void rsg_err(SrcLoc loc, const char *fmt, ...);
/** Emit "file:line:col: warning: ..." to stderr. */
void rsg_warn(SrcLoc loc, const char *fmt, ...);
/** Emit "fatal: ..." to stderr and terminate the process. */
noreturn void rsg_fatal(const char *fmt, ...);

#endif // RSG_COMMON_H
