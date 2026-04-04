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

/** Global error count - checked by the driver to decide exit status. */
static int32_t g_error_count = 0;

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
    g_error_count++;
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
