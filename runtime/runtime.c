#include "runtime.h"

// ------------------------------------------------------------------------
// Checked allocation helpers
// ------------------------------------------------------------------------
static void *checked_malloc(size_t size) {
    void *ptr = malloc(size);
    if (ptr == NULL) {
        fprintf(stderr, "fatal: out of memory\n");
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        exit(1);
    }
    return ptr;
}

static void *checked_realloc(void *ptr, size_t size) {
    void *result = realloc(ptr, size);
    if (result == NULL) {
        fprintf(stderr, "fatal: out of memory\n");
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        exit(1);
    }
    return result;
}

static RgStr rg_str_from_fmt(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int32_t length = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    char *buffer = checked_malloc(length + 1);
    va_start(args, fmt);
    vsnprintf(buffer, length + 1, fmt, args);
    va_end(args);
    return (RgStr){
        .data = buffer,
        .length = length,
        .reference_count = 1,
    };
}

// ------------------------------------------------------------------------
// String
// ------------------------------------------------------------------------
RgStr rg_str_lit(const char *s) {
    return (RgStr){
        .data = s,
        .length = (int32_t)strlen(s),
        .reference_count = -1, // static
    };
}

RgStr rg_str_new(const char *s, int32_t length) {
    char *buffer = checked_malloc(length + 1);
    memcpy(buffer, s, length);
    buffer[length] = '\0';
    return (RgStr){
        .data = buffer,
        .length = length,
        .reference_count = 1,
    };
}

RgStr rg_str_empty(void) {
    return rg_str_lit("");
}

RgStr rg_str_concat(RgStr a, RgStr b) {
    int32_t length = a.length + b.length;
    char *buffer = checked_malloc(length + 1);
    memcpy(buffer, a.data, a.length);
    memcpy(buffer + a.length, b.data, b.length);
    buffer[length] = '\0';
    return (RgStr){
        .data = buffer,
        .length = length,
        .reference_count = 1,
    };
}

RgStr rg_str_from_i32(int32_t v) {
    return rg_str_from_fmt("%d", v);
}

RgStr rg_str_from_u32(uint32_t v) {
    return rg_str_from_fmt("%u", v);
}

RgStr rg_str_from_f64(double v) {
    return rg_str_from_fmt("%g", v);
}

RgStr rg_str_from_bool(bool v) {
    return rg_str_lit(v ? "true" : "false");
}

// ------------------------------------------------------------------------
// String builder
// ------------------------------------------------------------------------
void rg_sb_init(RgStrBuilder *sb) {
    sb->capacity = 64;
    sb->length = 0;
    sb->buffer = checked_malloc(sb->capacity);
}

void rg_sb_append(RgStrBuilder *sb, const char *s, int32_t length) {
    while (sb->length + length >= sb->capacity) {
        sb->capacity *= 2;
        sb->buffer = checked_realloc(sb->buffer, sb->capacity);
    }
    memcpy(sb->buffer + sb->length, s, length);
    sb->length += length;
}

void rg_sb_append_str(RgStrBuilder *sb, RgStr s) {
    rg_sb_append(sb, s.data, s.length);
}

RgStr rg_sb_finish(RgStrBuilder *sb) {
    RgStr result = rg_str_new(sb->buffer, sb->length);
    free(sb->buffer);
    sb->buffer = NULL;
    sb->length = sb->capacity = 0;
    return result;
}

// ------------------------------------------------------------------------
// String comparison
// ------------------------------------------------------------------------
bool rg_str_eq(RgStr a, RgStr b) {
    if (a.length != b.length) {
        return false;
    }
    return memcmp(a.data, b.data, a.length) == 0;
}

// ------------------------------------------------------------------------
// Assert
// ------------------------------------------------------------------------
void rg_assert(bool cond, const char *msg, const char *file, int32_t line) {
    if (!cond) {
        if (msg != NULL) {
            fprintf(stderr, "assertion failed at %s:%d: %s\n", file, line, msg);
        } else {
            fprintf(stderr, "assertion failed at %s:%d\n", file, line);
        }
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        exit(1);
    }
}

// ------------------------------------------------------------------------
// I/O
// ------------------------------------------------------------------------
void rg_print_str(RgStr s) {
    fwrite(s.data, 1, s.length, stdout);
}

void rg_print_i32(int32_t v) {
    printf("%d", v);
}

void rg_print_u32(uint32_t v) {
    printf("%u", v);
}

void rg_print_f64(double v) {
    printf("%g", v);
}

void rg_print_bool(bool v) {
    printf("%s", v ? "true" : "false");
}
