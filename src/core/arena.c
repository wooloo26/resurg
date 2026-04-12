#include "common.h"

// ── Arena implementation ───────────────────────────────────────────────

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
    void *ptr = arena->current->data + arena->current->used;
    arena->current->used += size;
    return ptr;
}

void *arena_alloc_zero(Arena *arena, size_t size) {
    void *ptr = arena_alloc(arena, size);
    memset(ptr, 0, size);
    return ptr;
}

char *arena_strdup(Arena *arena, const char *src) {
    size_t len = strlen(src);
    char *duplicate = arena_alloc(arena, len + 1);
    memcpy(duplicate, src, len + 1);
    return duplicate;
}

char *arena_strndup(Arena *arena, const char *src, size_t len) {
    char *duplicate = arena_alloc(arena, len + 1);
    memcpy(duplicate, src, len);
    duplicate[len] = '\0';
    return duplicate;
}

char *arena_sprintf(Arena *arena, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int32_t len = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    char *buf = arena_alloc(arena, len + 1);
    va_start(args, fmt);
    vsnprintf(buf, len + 1, fmt, args);
    va_end(args);
    return buf;
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
