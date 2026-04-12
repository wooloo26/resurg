#ifndef RSG_HASH_TABLE_H
#define RSG_HASH_TABLE_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @file hash_table.h
 * @brief Str-keyed hash table with open addressing and linear probing.
 */

typedef struct Arena Arena;

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

#endif // RSG_HASH_TABLE_H
