#ifndef RSG_BUF_H
#define RSG_BUF_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/**
 * @file buf.h
 * @brief Stretchy buf (type-safe dynamic array) and arena-backed variant.
 */

typedef struct Arena Arena;

/**
 * Stretchy buf - type-safe dynamic array via macros.
 *
 * Declare as a typed NULL ptr, grow with BUF_PUSH, and free with
 * BUF_FREE.  The header lives just before the data ptr.
 *
 * @code
 *     Token *tokens = NULL;
 *     BUF_PUSH(tokens, tok);
 *     for (int i = 0; i < BUF_LEN(tokens); i++) { ... }
 *     BUF_FREE(tokens);
 * @endcode
 */
typedef struct {
    size_t len;
    size_t capacity;
} BufHeader;

#define BUF__HEADER(buf) ((BufHeader *)((char *)(buf) - sizeof(BufHeader)))
// Cast to int32_t: enables signed loop idioms (e.g. reverse iteration).
// Overflow is guarded at growth time in buf__grow().
#define BUF_LEN(buf) ((buf) != NULL ? (int32_t)BUF__HEADER(buf)->len : 0)
#define BUF_CAPACITY(buf) ((buf) != NULL ? (int32_t)BUF__HEADER(buf)->capacity : 0)
#define BUF_END(buf) ((buf) + BUF_LEN(buf))
#define BUF_FREE(buf) ((buf) != NULL ? (free(BUF__HEADER(buf)), (buf) = NULL) : 0)

// __typeof__ is a reserved-namespace extension supported by GCC/Clang;
// it avoids an unsafe (void *) cast for multi-level ptrs.
#define BUF_FIT(buf, needed)                                                                       \
    ((needed) <= BUF_CAPACITY(buf)                                                                 \
         ? 0                                                                                       \
         : ((buf) = (__typeof__(buf))buf__grow(                                                    \
                (const void *)(buf), (needed),                                                     \
                sizeof(*(buf))))) /* NOLINT(bugprone-sizeof-expression) */

#define BUF_PUSH(buf, value)                                                                       \
    do {                                                                                           \
        BUF_FIT((buf), BUF_LEN(buf) + 1);                                                          \
        (buf)[BUF__HEADER(buf)->len++] = (value);                                                  \
    } while (0)

/** Internal growth routine for stretchy bufs - do not call directly. */
void *buf__grow(const void *buf, size_t new_len, size_t elem_size);

// ── Arena-backed stretchy buf ───────────────────────────────────────

/**
 * Arena-backed stretchy buf - same layout as heap bufs, but allocated
 * from an Arena.  Never needs BUF_FREE; the memory is reclaimed when
 * the arena is destroyed.
 *
 * BUF_LEN, BUF_CAPACITY, and BUF_END work transparently.
 *
 * @code
 *     ASTNode **nodes = NULL;
 *     ARENA_BUF_PUSH(arena, nodes, node);
 *     for (int i = 0; i < BUF_LEN(nodes); i++) { ... }
 *     // no BUF_FREE needed
 * @endcode
 */

/** Internal arena growth routine - do not call directly. */
void *arena_buf__grow(Arena *arena, const void *buf, size_t new_len, size_t elem_size);

#define ARENA_BUF_FIT(arena, buf, needed)                                                          \
    ((needed) <= BUF_CAPACITY(buf)                                                                 \
         ? 0                                                                                       \
         : ((buf) = (__typeof__(buf))arena_buf__grow(                                              \
                (arena), (const void *)(buf), (needed),                                            \
                sizeof(*(buf))))) /* NOLINT(bugprone-sizeof-expression) */

#define ARENA_BUF_PUSH(arena, buf, value)                                                          \
    do {                                                                                           \
        ARENA_BUF_FIT((arena), (buf), BUF_LEN(buf) + 1);                                           \
        (buf)[BUF__HEADER(buf)->len++] = (value);                                                  \
    } while (0)

#endif // RSG_BUF_H
