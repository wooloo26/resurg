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
 * @brief Shared infrastructure: arena allocator, stretchy bufs, diagnostics.
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

// ── Hash table ─────────────────────────────────────────────────────────

/** Entry in an open-addressed hash table. */
typedef struct {
    const char *key; // NULL = empty slot, HASH_TABLE_TOMBSTONE = deleted
    void *value;
} HashEntry;

/**
 * Str-keyed hash table with open addressing and linear probing.
 *
 * Entries can live on the heap (arena == NULL) or in an Arena.  Heap-backed
 * tables must be cleaned up with hash_table_destroy(); arena-backed ones
 * are freed automatically when the arena is destroyed.
 */
typedef struct {
    HashEntry *entries;
    int32_t count;
    int32_t capacity;
    Arena *arena; // non-NULL → entries arena-allocated
} HashTable;

/** Initialise an empty table.  Pass NULL for @p arena to use the heap. */
void hash_table_init(HashTable *table, Arena *arena);
/** Free heap-allocated entries (no-op when arena-backed). */
void hash_table_destroy(HashTable *table);
/** Insert or update @p key → @p value. */
void hash_table_insert(HashTable *table, const char *key, void *value);
/** Look up @p key.  Returns the stored value or NULL. */
void *hash_table_lookup(const HashTable *table, const char *key);
/** Remove @p key from the table.  Returns true if the key was found. */
bool hash_table_remove(HashTable *table, const char *key);

// ── Stretchy buf ────────────────────────────────────────────────────

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
