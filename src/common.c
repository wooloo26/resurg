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
    ArenaBlock *block = calloc(1, sizeof(*block));
    if (block == NULL) {
        rg_fatal("out of memory");
    }
    block->data = malloc(capacity);
    if (block->data == NULL) {
        rg_fatal("out of memory");
    }
    block->used = 0;
    block->capacity = capacity;
    block->next = NULL;
    return block;
}

Arena *arena_create(void) {
    Arena *arena = malloc(sizeof(*arena));
    if (arena == NULL) {
        rg_fatal("out of memory");
    }
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

/**
 * Grow a stretchy buffer so it can hold at least @p new_length elements of
 * @p element_size bytes each.  Returns the new data pointer (header hidden
 * before it).
 */
void *buffer__grow(const void *buffer, size_t new_length, size_t element_size) {
    size_t new_capacity = BUFFER_CAPACITY(buffer) ? BUFFER_CAPACITY(buffer) * 2 : 16;
    if (new_capacity < new_length) {
        new_capacity = new_length;
    }

    size_t new_size = sizeof(BufferHeader) + new_capacity * element_size;
    BufferHeader *header;
    if (buffer != NULL) {
        header = realloc(BUFFER__HEADER(buffer), new_size);
    } else {
        header = malloc(new_size);
        header->length = 0;
    }
    if (header == NULL) {
        rg_fatal("out of memory");
    }
    header->capacity = new_capacity;
    return (char *)header + sizeof(BufferHeader);
}

/** Global error count - checked by the driver to decide exit status. */
static int32_t g_error_count = 0;

void rg_error(SourceLocation location, const char *format, ...) {
    fprintf(stderr, "%s:%d:%d: error: ", location.file, location.line, location.column);
    va_list arguments;
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    fprintf(stderr, "\n");
    g_error_count++;
}

void rg_warn(SourceLocation location, const char *format, ...) {
    fprintf(stderr, "%s:%d:%d: warning: ", location.file, location.line, location.column);
    va_list arguments;
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    fprintf(stderr, "\n");
}

noreturn void rg_fatal(const char *format, ...) {
    fprintf(stderr, "fatal: ");
    va_list arguments;
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    fprintf(stderr, "\n");
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    exit(1);
}
