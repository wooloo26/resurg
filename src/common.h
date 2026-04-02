#ifndef RG_COMMON_H
#define RG_COMMON_H

#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>

// ------------------------------------------------------------------------
// Arena allocator — bump-pointer allocation for AST nodes, tokens, strings.
// Freed in one shot at end of compilation.
// ------------------------------------------------------------------------
#define ARENA_BLOCK_SIZE ((size_t)1024 * 1024) // 1 MiB

typedef struct Arena Arena;

// Create a new arena with a default-sized initial block.
Arena *arena_create(void);
// Free all blocks and the arena itself.
void arena_destroy(Arena *a);
// Allocate size bytes (8-byte aligned) from the arena.
void *arena_alloc(Arena *a, size_t size);
// Duplicate a NUL-terminated string into the arena.
char *arena_strdup(Arena *a, const char *s);
// Duplicate the first len bytes of s into the arena.
char *arena_strndup(Arena *a, const char *s, size_t len);
// Format a string into the arena (printf-style).
char *arena_sprintf(Arena *a, const char *fmt, ...);

// ------------------------------------------------------------------------
// Stretchy buffer — type-safe dynamic array via macros.
// Usage:
//   Token *tokens = NULL;
//   BUF_PUSH(tokens, tok);
//   for (int i = 0; i < BUF_LEN(tokens); i++) { ... }
//   BUF_FREE(tokens);
// ------------------------------------------------------------------------
typedef struct {
    size_t len;
    size_t cap;
} BufHeader;

#define BUF__HDR(b) ((BufHeader *)((char *)(b) - sizeof(BufHeader)))
#define BUF_LEN(b) ((b) != NULL ? (int32_t)BUF__HDR(b)->len : 0)
#define BUF_CAP(b) ((b) != NULL ? (int32_t)BUF__HDR(b)->cap : 0)
#define BUF_END(b) ((b) + BUF_LEN(b))
#define BUF_FREE(b) ((b) != NULL ? (free(BUF__HDR(b)), (b) = NULL) : 0)

// NOLINTNEXTLINE(bugprone-sizeof-expression)
#define BUF_FIT(b, n) ((n) <= BUF_CAP(b) ? 0 : ((b) = (__typeof__(b))buf__grow((const void *)(b), (n), sizeof(*(b)))))

#define BUF_PUSH(b, v)                                                                                                 \
    do {                                                                                                               \
        BUF_FIT((b), BUF_LEN(b) + 1);                                                                                  \
        (b)[BUF__HDR(b)->len++] = (v);                                                                                 \
    } while (0)

// Grow a stretchy buffer to at least new_len elements.
void *buf__grow(const void *buf, size_t new_len, size_t elem_size);

// ------------------------------------------------------------------------
// Diagnostics — error/warning reporting with source location.
// ------------------------------------------------------------------------
typedef struct {
    const char *file;
    int32_t line;
    int32_t col;
} SrcLoc;

// Report an error at the given source location.
void rg_error(SrcLoc loc, const char *fmt, ...);
// Report a warning at the given source location.
void rg_warn(SrcLoc loc, const char *fmt, ...);
// Print a fatal error and abort.
noreturn void rg_fatal(const char *fmt, ...);

#endif // RG_COMMON_H
