#ifndef RSG_ARENA_H
#define RSG_ARENA_H

#include <stdarg.h>
#include <stddef.h>

/**
 * @file arena.h
 * @brief Bump-ptr arena allocator.
 */

/** Block size for the arena bump allocator (1 MiB). */
#define ARENA_BLOCK_SIZE ((size_t)1024 * 1024)

/**
 * Opaque bump-ptr arena.  All AST nodes, tokens, and interned strs
 * are allocated here and freed in one shot at end of compilation.
 */
typedef struct Arena Arena;

/** Create a new arena with one pre-allocated block. */
Arena *arena_create(void);
/** Free every block owned by @p arena. */
void arena_destroy(Arena *arena);
/** Allocate @p size bytes from @p arena, 8-byte aligned. */
void *arena_alloc(Arena *arena, size_t size);
/** Allocate @p size zero-initialised bytes from @p arena. */
void *arena_alloc_zero(Arena *arena, size_t size);
/** Duplicate a NUL-terminated str into @p arena. */
char *arena_strdup(Arena *arena, const char *src);
/** Duplicate the first @p len bytes of @p src into @p arena. */
char *arena_strndup(Arena *arena, const char *src, size_t len);
/** printf-style fmtting into arena-allocated memory. */
char *arena_sprintf(Arena *arena, const char *fmt, ...);

#endif // RSG_ARENA_H
