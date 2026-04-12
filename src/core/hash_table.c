#include "common.h"

// ── Hash table (open addressing, linear probing, FNV-1a) ───────────────

#define HASH_TABLE_INITIAL_CAPACITY 16

/** Tombstone marker for deleted slots (invalid ptr, never derefd). */
static const char *const HASH_TABLE_TOMBSTONE = (const char *)(uintptr_t)1;

/** FNV-1a hash for NUL-terminated strs. */
static uint32_t hash_str(const char *key) {
    uint32_t hash = 2166136261u;
    for (const char *p = key; *p != '\0'; p++) {
        hash ^= (uint8_t)*p;
        hash *= 16777619u;
    }
    return hash;
}

/** Allocate a zeroed entries array (heap or arena). */
static HashEntry *hash_table__alloc_entries(Arena *arena, int32_t capacity) {
    size_t bytes = (size_t)capacity * sizeof(HashEntry);
    if (arena != NULL) {
        HashEntry *entries = arena_alloc(arena, bytes);
        memset(entries, 0, bytes);
        return entries;
    }
    return rsg_calloc(capacity, sizeof(HashEntry));
}

/** Rehash into a table of @p new_capacity (must be power of 2). */
static void hash_table__resize(HashTable *table, int32_t new_capacity) {
    HashEntry *new_entries = hash_table__alloc_entries(table->arena, new_capacity);
    uint32_t mask = (uint32_t)(new_capacity - 1);

    for (int32_t i = 0; i < table->capacity; i++) {
        if (table->entries[i].key != NULL && table->entries[i].key != HASH_TABLE_TOMBSTONE) {
            uint32_t idx = hash_str(table->entries[i].key) & mask;
            while (new_entries[idx].key != NULL) {
                idx = (idx + 1) & mask;
            }
            new_entries[idx] = table->entries[i];
        }
    }

    if (table->arena == NULL) {
        free(table->entries);
    }
    table->entries = new_entries;
    table->capacity = new_capacity;
}

void hash_table_init(HashTable *table, Arena *arena) {
    table->entries = NULL;
    table->count = 0;
    table->capacity = 0;
    table->arena = arena;
}

void hash_table_destroy(HashTable *table) {
    if (table->arena == NULL) {
        free(table->entries);
    }
    table->entries = NULL;
    table->count = 0;
    table->capacity = 0;
}

/**
 * Probe the table for @p key, skipping tombstones.
 * Returns a ptr to the matching entry, or NULL if not found.
 */
static HashEntry *hash_table__find_entry(const HashTable *table, const char *key) {
    uint32_t mask = (uint32_t)(table->capacity - 1);
    uint32_t idx = hash_str(key) & mask;

    while (table->entries[idx].key != NULL) {
        if (table->entries[idx].key != HASH_TABLE_TOMBSTONE &&
            strcmp(table->entries[idx].key, key) == 0) {
            return &table->entries[idx];
        }
        idx = (idx + 1) & mask;
    }
    return NULL;
}

void hash_table_insert(HashTable *table, const char *key, void *value) {
    if (table->capacity == 0 || table->count * 4 >= table->capacity * 3) {
        int32_t new_capacity =
            table->capacity == 0 ? HASH_TABLE_INITIAL_CAPACITY : table->capacity * 2;
        hash_table__resize(table, new_capacity);
    }

    // Check for existing key first
    HashEntry *existing = hash_table__find_entry(table, key);
    if (existing != NULL) {
        existing->value = value;
        return;
    }

    // Insert into first available slot (empty or tombstone)
    uint32_t mask = (uint32_t)(table->capacity - 1);
    uint32_t idx = hash_str(key) & mask;

    while (table->entries[idx].key != NULL && table->entries[idx].key != HASH_TABLE_TOMBSTONE) {
        idx = (idx + 1) & mask;
    }

    table->entries[idx].key = key;
    table->entries[idx].value = value;
    table->count++;
}

void *hash_table_lookup(const HashTable *table, const char *key) {
    if (table->capacity == 0) {
        return NULL;
    }
    HashEntry *entry = hash_table__find_entry(table, key);
    return entry != NULL ? entry->value : NULL;
}

bool hash_table_remove(HashTable *table, const char *key) {
    if (table->capacity == 0) {
        return false;
    }
    HashEntry *entry = hash_table__find_entry(table, key);
    if (entry == NULL) {
        return false;
    }
    entry->key = HASH_TABLE_TOMBSTONE;
    entry->value = NULL;
    table->count--;
    return true;
}
