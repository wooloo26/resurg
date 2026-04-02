#include "runtime.h"

// ---------------------------------------------------------------------------
// Checked allocation helpers
// ---------------------------------------------------------------------------
static void *xmalloc(size_t size) {
    void *p = malloc(size);
    if (p == NULL) {
        fprintf(stderr, "fatal: out of memory\n");
        exit(1); // NOLINT(concurrency-mt-unsafe)
    }
    return p;
}

static void *xrealloc(void *ptr, size_t size) {
    void *p = realloc(ptr, size);
    if (p == NULL) {
        fprintf(stderr, "fatal: out of memory\n");
        exit(1); // NOLINT(concurrency-mt-unsafe)
    }
    return p;
}

static RgStr rg_str_from_fmt(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int32_t len = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    char *buf = xmalloc(len + 1);
    va_start(args, fmt);
    vsnprintf(buf, len + 1, fmt, args);
    va_end(args);
    return (RgStr){
        .data = buf,
        .len = len,
        .refcount = 1,
    };
}

// ---------------------------------------------------------------------------
// String
// ---------------------------------------------------------------------------
RgStr rg_str_lit(const char *s) {
    return (RgStr){
        .data = s,
        .len = (int32_t)strlen(s),
        .refcount = -1, // static
    };
}

RgStr rg_str_new(const char *s, int32_t len) {
    char *buf = xmalloc(len + 1);
    memcpy(buf, s, len);
    buf[len] = '\0';
    return (RgStr){
        .data = buf,
        .len = len,
        .refcount = 1,
    };
}

RgStr rg_str_empty(void) {
    return rg_str_lit("");
}

RgStr rg_str_concat(RgStr a, RgStr b) {
    int32_t len = a.len + b.len;
    char *buf = xmalloc(len + 1);
    memcpy(buf, a.data, a.len);
    memcpy(buf + a.len, b.data, b.len);
    buf[len] = '\0';
    return (RgStr){
        .data = buf,
        .len = len,
        .refcount = 1,
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

// ---------------------------------------------------------------------------
// String builder
// ---------------------------------------------------------------------------
void rg_sb_init(RgStrBuilder *sb) {
    sb->cap = 64;
    sb->len = 0;
    sb->buf = xmalloc(sb->cap);
}

void rg_sb_append(RgStrBuilder *sb, const char *s, int32_t len) {
    while (sb->len + len >= sb->cap) {
        sb->cap *= 2;
        sb->buf = xrealloc(sb->buf, sb->cap);
    }
    memcpy(sb->buf + sb->len, s, len);
    sb->len += len;
}

void rg_sb_append_str(RgStrBuilder *sb, RgStr s) {
    rg_sb_append(sb, s.data, s.len);
}

RgStr rg_sb_finish(RgStrBuilder *sb) {
    RgStr r = rg_str_new(sb->buf, sb->len);
    free(sb->buf);
    sb->buf = NULL;
    sb->len = sb->cap = 0;
    return r;
}

// ---------------------------------------------------------------------------
// String comparison
// ---------------------------------------------------------------------------
bool rg_str_eq(RgStr a, RgStr b) {
    if (a.len != b.len) {
        return false;
    }
    return memcmp(a.data, b.data, a.len) == 0;
}

// ---------------------------------------------------------------------------
// Assert
// ---------------------------------------------------------------------------
void rg_assert(bool cond, const char *msg, const char *file, int32_t line) {
    if (!cond) {
        if (msg != NULL) {
            fprintf(stderr, "assertion failed at %s:%d: %s\n", file, line, msg);
        } else {
            fprintf(stderr, "assertion failed at %s:%d\n", file, line);
        }
        exit(1); // NOLINT(concurrency-mt-unsafe)
    }
}

// ---------------------------------------------------------------------------
// I/O
// ---------------------------------------------------------------------------
void rg_print_str(RgStr s) {
    fwrite(s.data, 1, s.len, stdout);
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
