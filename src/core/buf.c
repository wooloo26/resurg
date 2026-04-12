#include "common.h"

// ── Stretchy buf implementation ─────────────────────────────────────

/**
 * Grow a stretchy buf so it can hold at least @p new_len elems of
 * @p elem_size bytes each.  Returns the new data ptr (header hidden
 * before it).
 */
void *buf__grow(const void *buf, size_t new_len, size_t elem_size) {
    static_assert(sizeof(size_t) >= sizeof(int32_t),
                  "BUF_LEN assumes size_t is at least as wide as int32_t");

    size_t new_capacity = BUF_CAPACITY(buf) ? BUF_CAPACITY(buf) * 2 : 16;
    if (new_capacity < new_len) {
        new_capacity = new_len;
    }
    if (new_capacity > INT32_MAX) {
        rsg_fatal("buf capacity exceeds INT32_MAX");
    }

    size_t new_size = sizeof(BufHeader) + new_capacity * elem_size;
    BufHeader *header;
    if (buf != NULL) {
        header = rsg_realloc(BUF__HEADER(buf), new_size);
    } else {
        header = rsg_malloc(new_size);
        header->len = 0;
    }
    header->capacity = new_capacity;
    return (char *)header + sizeof(BufHeader);
}

/**
 * Arena-backed growth: allocate a new header+data region from @p arena,
 * copy existing elements, and return the new data ptr.  The old
 * allocation is abandoned (freed when the arena is destroyed).
 */
void *arena_buf__grow(Arena *arena, const void *buf, size_t new_len, size_t elem_size) {
    size_t old_len = buf != NULL ? BUF__HEADER(buf)->len : 0;
    size_t new_capacity = BUF_CAPACITY(buf) ? (size_t)BUF_CAPACITY(buf) * 2 : 16;
    if (new_capacity < new_len) {
        new_capacity = new_len;
    }
    if (new_capacity > INT32_MAX) {
        rsg_fatal("arena buf capacity exceeds INT32_MAX");
    }

    size_t new_size = sizeof(BufHeader) + new_capacity * elem_size;
    BufHeader *header = arena_alloc(arena, new_size);
    header->len = old_len;
    header->capacity = new_capacity;

    char *new_data = (char *)header + sizeof(BufHeader);
    if (buf != NULL && old_len > 0) {
        memcpy(new_data, buf, old_len * elem_size);
    }
    return new_data;
}
