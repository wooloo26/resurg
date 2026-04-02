#include "common.h"

// ------------------------------------------------------------------------
// ArenaBlock — internal implementation
// ------------------------------------------------------------------------
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

// ------------------------------------------------------------------------
// Arena allocator
// ------------------------------------------------------------------------
static ArenaBlock *arena_block_new(size_t capacity) {
    ArenaBlock *b = calloc(1, sizeof(*b));
    if (b == NULL) {
        rg_fatal("out of memory");
    }
    b->data = malloc(capacity);
    if (b->data == NULL) {
        rg_fatal("out of memory");
    }
    b->used = 0;
    b->capacity = capacity;
    b->next = NULL;
    return b;
}

Arena *arena_create(void) {
    Arena *a = malloc(sizeof(*a));
    if (a == NULL) {
        rg_fatal("out of memory");
    }
    a->head = arena_block_new(ARENA_BLOCK_SIZE);
    a->current = a->head;
    return a;
}

void *arena_alloc(Arena *a, size_t size) {
    // Align to 8 bytes
    size = (size + 7) & ~(size_t)7;

    if (a->current->used + size > a->current->capacity) {
        size_t cap = size > ARENA_BLOCK_SIZE ? size : ARENA_BLOCK_SIZE;
        ArenaBlock *b = arena_block_new(cap);
        a->current->next = b;
        a->current = b;
    }
    void *ptr = a->current->data + a->current->used;
    a->current->used += size;
    return ptr;
}

char *arena_strdup(Arena *a, const char *s) {
    size_t len = strlen(s);
    char *dup = arena_alloc(a, len + 1);
    memcpy(dup, s, len + 1);
    return dup;
}

char *arena_strndup(Arena *a, const char *s, size_t len) {
    char *dup = arena_alloc(a, len + 1);
    memcpy(dup, s, len);
    dup[len] = '\0';
    return dup;
}

char *arena_sprintf(Arena *a, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int32_t len = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    char *buf = arena_alloc(a, len + 1);
    va_start(args, fmt);
    vsnprintf(buf, len + 1, fmt, args);
    va_end(args);
    return buf;
}

void arena_destroy(Arena *a) {
    ArenaBlock *b = a->head;
    while (b != NULL) {
        ArenaBlock *next = b->next;
        free(b->data);
        free(b);
        b = next;
    }
    free(a);
}

// ------------------------------------------------------------------------
// Stretchy buffer
// ------------------------------------------------------------------------
void *buf__grow(const void *buf, size_t new_len, size_t elem_size) {
    size_t new_cap = BUF_CAP(buf) ? BUF_CAP(buf) * 2 : 16;
    if (new_cap < new_len) {
        new_cap = new_len;
    }

    size_t new_size = sizeof(BufHeader) + new_cap * elem_size;
    BufHeader *hdr;
    if (buf != NULL) {
        hdr = realloc(BUF__HDR(buf), new_size);
    } else {
        hdr = malloc(new_size);
        hdr->len = 0;
    }
    if (hdr == NULL) {
        rg_fatal("out of memory");
    }
    hdr->cap = new_cap;
    return (char *)hdr + sizeof(BufHeader);
}

// ------------------------------------------------------------------------
// Diagnostics
// ------------------------------------------------------------------------
static int32_t g_error_count = 0;

void rg_error(SrcLoc loc, const char *fmt, ...) {
    fprintf(stderr, "%s:%d:%d: error: ", loc.file, loc.line, loc.col);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
    g_error_count++;
}

void rg_warn(SrcLoc loc, const char *fmt, ...) {
    fprintf(stderr, "%s:%d:%d: warning: ", loc.file, loc.line, loc.col);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

noreturn void rg_fatal(const char *fmt, ...) {
    fprintf(stderr, "fatal: ");
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    exit(1);
}
