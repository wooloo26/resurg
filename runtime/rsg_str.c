#include "rsg_str.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rsg_internal.h"

// ── printf-style helper ────────────────────────────────────────────────

/** Build an RsgStr via printf-style fmtting. */
static RsgStr rsg_str_from_fmt(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int32_t len = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    char *buf = checked_malloc(len + 1);
    va_start(args, fmt);
    vsnprintf(buf, len + 1, fmt, args);
    va_end(args);
    return (RsgStr){
        .data = buf,
        .len = len,
        .ref_count = 1,
    };
}

// ── Str constructors and conversions ───────────────────────────────────

RsgStr rsg_str_lit(const char *src) {
    return (RsgStr){
        .data = src,
        .len = (int32_t)strlen(src),
        .ref_count = -1, // static
    };
}

RsgStr rsg_str_new(const char *src, int32_t len) {
    char *buf = checked_malloc(len + 1);
    memcpy(buf, src, len);
    buf[len] = '\0';
    return (RsgStr){
        .data = buf,
        .len = len,
        .ref_count = 1,
    };
}

RsgStr rsg_str_empty(void) {
    return rsg_str_lit("");
}

RsgStr rsg_str_concat(RsgStr left, RsgStr right) {
    if (left.len > INT32_MAX - right.len) {
        fprintf(stderr, "fatal: str concatenation overflow\n");
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        exit(1);
    }
    int32_t len = left.len + right.len;
    char *buf = checked_malloc(len + 1);
    memcpy(buf, left.data, left.len);
    memcpy(buf + left.len, right.data, right.len);
    buf[len] = '\0';
    return (RsgStr){
        .data = buf,
        .len = len,
        .ref_count = 1,
    };
}

RsgStr rsg_str_from_i32(int32_t value) {
    return rsg_str_from_fmt("%d", value);
}

RsgStr rsg_str_from_u32(uint32_t value) {
    return rsg_str_from_fmt("%u", value);
}

RsgStr rsg_str_from_i64(int64_t value) {
    return rsg_str_from_fmt("%lld", (long long)value);
}

RsgStr rsg_str_from_u64(uint64_t value) {
    return rsg_str_from_fmt("%llu", (unsigned long long)value);
}

RsgStr rsg_str_from_f32(float value) {
    return rsg_str_from_fmt("%g", (double)value);
}

RsgStr rsg_str_from_f64(double value) {
    return rsg_str_from_fmt("%g", value);
}

RsgStr rsg_str_from_bool(bool value) {
    return rsg_str_lit(value ? "true" : "false");
}

RsgStr rsg_str_from_char(char value) {
    char buf[2] = {value, '\0'};
    return rsg_str_new(buf, 1);
}

// ── Str builder ────────────────────────────────────────────────────────

void rsg_str_builder_init(RsgStrBuilder *builder) {
    builder->capacity = 64;
    builder->len = 0;
    builder->buf = checked_malloc(builder->capacity);
}

void rsg_str_builder_append(RsgStrBuilder *builder, const char *src, int32_t len) {
    while (builder->len + len >= builder->capacity) {
        builder->capacity *= 2;
        builder->buf = checked_realloc(builder->buf, builder->capacity);
    }
    memcpy(builder->buf + builder->len, src, len);
    builder->len += len;
}

void rsg_str_builder_append_str(RsgStrBuilder *builder, RsgStr src) {
    rsg_str_builder_append(builder, src.data, src.len);
}

RsgStr rsg_str_builder_finish(RsgStrBuilder *builder) {
    RsgStr result = rsg_str_new(builder->buf, builder->len);
    free(builder->buf);
    builder->buf = NULL;
    builder->len = builder->capacity = 0;
    return result;
}

bool rsg_str_equal(RsgStr left, RsgStr right) {
    if (left.len != right.len) {
        return false;
    }
    return memcmp(left.data, right.data, left.len) == 0;
}
