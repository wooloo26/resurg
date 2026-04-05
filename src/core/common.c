#include "common.h"

/** Single contiguous memory block within an Arena. */
typedef struct ArenaBlock ArenaBlock;

struct ArenaBlock {
    uint8_t *data;
    size_t used;
    size_t capacity;
    struct ArenaBlock *next;
};

struct Arena {
    ArenaBlock *head;
    ArenaBlock *current;
};

/** Allocate a new ArenaBlock with the given byte @p capacity. */
static ArenaBlock *arena_block_new(size_t capacity) {
    ArenaBlock *block = rsg_calloc(1, sizeof(*block));
    block->data = rsg_malloc(capacity);
    block->used = 0;
    block->capacity = capacity;
    block->next = NULL;
    return block;
}

Arena *arena_create(void) {
    Arena *arena = rsg_malloc(sizeof(*arena));
    arena->head = arena_block_new(ARENA_BLOCK_SIZE);
    arena->current = arena->head;
    return arena;
}

void *arena_alloc(Arena *arena, size_t size) {
    // Align to 8 bytes
    size = (size + 7) & ~(size_t)7;

    if (arena->current->used + size > arena->current->capacity) {
        size_t capacity = size > ARENA_BLOCK_SIZE ? size : ARENA_BLOCK_SIZE;
        ArenaBlock *block = arena_block_new(capacity);
        arena->current->next = block;
        arena->current = block;
    }
    void *pointer = arena->current->data + arena->current->used;
    arena->current->used += size;
    return pointer;
}

void *arena_alloc_zero(Arena *arena, size_t size) {
    void *pointer = arena_alloc(arena, size);
    memset(pointer, 0, size);
    return pointer;
}

char *arena_strdup(Arena *arena, const char *source) {
    size_t length = strlen(source);
    char *duplicate = arena_alloc(arena, length + 1);
    memcpy(duplicate, source, length + 1);
    return duplicate;
}

char *arena_strndup(Arena *arena, const char *source, size_t length) {
    char *duplicate = arena_alloc(arena, length + 1);
    memcpy(duplicate, source, length);
    duplicate[length] = '\0';
    return duplicate;
}

char *arena_sprintf(Arena *arena, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    int32_t length = vsnprintf(NULL, 0, format, arguments);
    va_end(arguments);
    char *buffer = arena_alloc(arena, length + 1);
    va_start(arguments, format);
    vsnprintf(buffer, length + 1, format, arguments);
    va_end(arguments);
    return buffer;
}

void arena_destroy(Arena *arena) {
    ArenaBlock *block = arena->head;
    while (block != NULL) {
        ArenaBlock *next = block->next;
        free(block->data);
        free(block);
        block = next;
    }
    free(arena);
}

// ── Hash table (open addressing, linear probing, FNV-1a) ───────────────

#define HASH_TABLE_INITIAL_CAPACITY 16

/** Tombstone marker for deleted slots (invalid pointer, never dereferenced). */
static const char *const HASH_TABLE_TOMBSTONE = (const char *)(uintptr_t)1;

/** FNV-1a hash for NUL-terminated strings. */
static uint32_t hash_string(const char *key) {
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
            uint32_t index = hash_string(table->entries[i].key) & mask;
            while (new_entries[index].key != NULL) {
                index = (index + 1) & mask;
            }
            new_entries[index] = table->entries[i];
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
 * Returns a pointer to the matching entry, or NULL if not found.
 */
static HashEntry *hash_table__find_entry(const HashTable *table, const char *key) {
    uint32_t mask = (uint32_t)(table->capacity - 1);
    uint32_t index = hash_string(key) & mask;

    while (table->entries[index].key != NULL) {
        if (table->entries[index].key != HASH_TABLE_TOMBSTONE &&
            strcmp(table->entries[index].key, key) == 0) {
            return &table->entries[index];
        }
        index = (index + 1) & mask;
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
    uint32_t index = hash_string(key) & mask;

    while (table->entries[index].key != NULL && table->entries[index].key != HASH_TABLE_TOMBSTONE) {
        index = (index + 1) & mask;
    }

    table->entries[index].key = key;
    table->entries[index].value = value;
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

/**
 * Grow a stretchy buffer so it can hold at least @p new_length elements of
 * @p element_size bytes each.  Returns the new data pointer (header hidden
 * before it).
 */
void *buffer__grow(const void *buffer, size_t new_length, size_t element_size) {
    static_assert(sizeof(size_t) >= sizeof(int32_t),
                  "BUFFER_LENGTH assumes size_t is at least as wide as int32_t");

    size_t new_capacity = BUFFER_CAPACITY(buffer) ? BUFFER_CAPACITY(buffer) * 2 : 16;
    if (new_capacity < new_length) {
        new_capacity = new_length;
    }
    if (new_capacity > INT32_MAX) {
        rsg_fatal("buffer capacity exceeds INT32_MAX");
    }

    size_t new_size = sizeof(BufferHeader) + new_capacity * element_size;
    BufferHeader *header;
    if (buffer != NULL) {
        header = rsg_realloc(BUFFER__HEADER(buffer), new_size);
    } else {
        header = rsg_malloc(new_size);
        header->length = 0;
    }
    header->capacity = new_capacity;
    return (char *)header + sizeof(BufferHeader);
}

// Checked allocation wrappers - abort on OOM.

void *rsg_malloc(size_t size) {
    void *pointer = malloc(size);
    if (pointer == NULL) {
        rsg_fatal("out of memory");
    }
    return pointer;
}

void *rsg_calloc(size_t count, size_t size) {
    void *pointer = calloc(count, size);
    if (pointer == NULL) {
        rsg_fatal("out of memory");
    }
    return pointer;
}

void *rsg_realloc(void *pointer, size_t size) {
    void *result = realloc(pointer, size);
    if (result == NULL) {
        rsg_fatal("out of memory");
    }
    return result;
}

/** Emit "label: msg\n" to @p stream with a location prefix. */
static void emit_located_diagnostic(SourceLocation location, const char *label, const char *format,
                                    va_list arguments) {
    fprintf(stderr, "%s:%d:%d: %s: ", location.file, location.line, location.column, label);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
}

void rsg_error(SourceLocation location, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    emit_located_diagnostic(location, "error", format, arguments);
    va_end(arguments);
}

void rsg_warn(SourceLocation location, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    emit_located_diagnostic(location, "warning", format, arguments);
    va_end(arguments);
}

noreturn void rsg_fatal(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    fputs("fatal: ", stderr);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    va_end(arguments);
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    exit(1);
}
