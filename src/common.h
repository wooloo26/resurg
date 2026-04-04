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

/**
 * @file common.h
 * @brief Shared infrastructure: arena allocator, stretchy buffers, diagnostics.
 */

/** Block size for the arena bump allocator (1 MiB). */
#define ARENA_BLOCK_SIZE ((size_t)1024 * 1024)

/**
 * Opaque bump-pointer arena.  All AST nodes, tokens, and interned strings
 * are allocated here and freed in one shot at end of compilation.
 */
typedef struct Arena Arena;

/** Create a new arena with one pre-allocated block. */
Arena *arena_create(void);
/** Free every block owned by @p arena. */
void arena_destroy(Arena *arena);
/** Allocate @p size bytes from @p arena, 8-byte aligned. */
void *arena_alloc(Arena *arena, size_t size);
/** Duplicate a NUL-terminated string into @p arena. */
char *arena_strdup(Arena *arena, const char *source);
/** Duplicate the first @p length bytes of @p source into @p arena. */
char *arena_strndup(Arena *arena, const char *source, size_t length);
/** printf-style formatting into arena-allocated memory. */
char *arena_sprintf(Arena *arena, const char *format, ...);

// ── Hash table ─────────────────────────────────────────────────────────

/** Entry in an open-addressed hash table. */
typedef struct {
    const char *key; // NULL = empty slot
    void *value;
} HashEntry;

/**
 * String-keyed hash table with open addressing and linear probing.
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

// ── Stretchy buffer ────────────────────────────────────────────────────

/**
 * Stretchy buffer - type-safe dynamic array via macros.
 *
 * Declare as a typed NULL pointer, grow with BUFFER_PUSH, and free with
 * BUFFER_FREE.  The header lives just before the data pointer.
 *
 * @code
 *     Token *tokens = NULL;
 *     BUFFER_PUSH(tokens, tok);
 *     for (int i = 0; i < BUFFER_LENGTH(tokens); i++) { ... }
 *     BUFFER_FREE(tokens);
 * @endcode
 */
typedef struct {
    size_t length;
    size_t capacity;
} BufferHeader;

#define BUFFER__HEADER(buffer) ((BufferHeader *)((char *)(buffer) - sizeof(BufferHeader)))
// Cast to int32_t: enables signed loop idioms (e.g. reverse iteration).
// Overflow is guarded at growth time in buffer__grow().
#define BUFFER_LENGTH(buffer) ((buffer) != NULL ? (int32_t)BUFFER__HEADER(buffer)->length : 0)
#define BUFFER_CAPACITY(buffer) ((buffer) != NULL ? (int32_t)BUFFER__HEADER(buffer)->capacity : 0)
#define BUFFER_END(buffer) ((buffer) + BUFFER_LENGTH(buffer))
#define BUFFER_FREE(buffer) ((buffer) != NULL ? (free(BUFFER__HEADER(buffer)), (buffer) = NULL) : 0)

// NOLINTNEXTLINE(bugprone-sizeof-expression)
#define BUFFER_FIT(buffer, needed)                                                                 \
    ((needed) <= BUFFER_CAPACITY(buffer)                                                           \
         ? 0                                                                                       \
         : ((buffer) = (__typeof__(buffer))buffer__grow((const void *)(buffer), (needed),          \
                                                        sizeof(*(buffer)))))

#define BUFFER_PUSH(buffer, value)                                                                 \
    do {                                                                                           \
        BUFFER_FIT((buffer), BUFFER_LENGTH(buffer) + 1);                                           \
        (buffer)[BUFFER__HEADER(buffer)->length++] = (value);                                      \
    } while (0)

/** Internal growth routine for stretchy buffers - do not call directly. */
void *buffer__grow(const void *buffer, size_t new_length, size_t element_size);

/** A file:line:column triple attached to tokens and AST nodes. */
typedef struct {
    const char *file;
    int32_t line;
    int32_t column;
} SourceLocation;

/** Checked malloc - aborts on OOM. */
void *rsg_malloc(size_t size);
/** Checked calloc - aborts on OOM. */
void *rsg_calloc(size_t count, size_t size);
/** Checked realloc - aborts on OOM. */
void *rsg_realloc(void *pointer, size_t size);

/**
 * Emit "file:line:col: error: ..." to stderr.
 */
void rsg_error(SourceLocation location, const char *format, ...);
/** Emit "file:line:col: warning: ..." to stderr. */
void rsg_warn(SourceLocation location, const char *format, ...);
/** Emit "fatal: ..." to stderr and terminate the process. */
noreturn void rsg_fatal(const char *format, ...);

#endif // RG_COMMON_H
