#include "rsg_runtime.h"

// Checked allocation helpers - abort on OOM.

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

/** Build an RsgString via printf-style formatting. */
static RsgString rsg_string_from_format(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    int32_t length = vsnprintf(NULL, 0, format, arguments);
    va_end(arguments);
    char *buffer = checked_malloc(length + 1);
    va_start(arguments, format);
    vsnprintf(buffer, length + 1, format, arguments);
    va_end(arguments);
    return (RsgString){
        .data = buffer,
        .length = length,
        .reference_count = 1,
    };
}

// String constructors and conversions.
//
// NOTE: Heap-allocated strings (reference_count == 1) are never freed.
// This is intentional — a tracing GC is planned for v0.4.0.  Until then,
// every rsg_string_new / rsg_string_concat leaks by design.

RsgString rsg_string_literal(const char *source) {
    return (RsgString){
        .data = source,
        .length = (int32_t)strlen(source),
        .reference_count = -1, // static
    };
}

RsgString rsg_string_new(const char *source, int32_t length) {
    char *buffer = checked_malloc(length + 1);
    memcpy(buffer, source, length);
    buffer[length] = '\0';
    return (RsgString){
        .data = buffer,
        .length = length,
        .reference_count = 1,
    };
}

RsgString rsg_string_empty(void) {
    return rsg_string_literal("");
}

RsgString rsg_string_concat(RsgString left, RsgString right) {
    if (left.length > INT32_MAX - right.length) {
        fprintf(stderr, "fatal: string concatenation overflow\n");
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        exit(1);
    }
    int32_t length = left.length + right.length;
    char *buffer = checked_malloc(length + 1);
    memcpy(buffer, left.data, left.length);
    memcpy(buffer + left.length, right.data, right.length);
    buffer[length] = '\0';
    return (RsgString){
        .data = buffer,
        .length = length,
        .reference_count = 1,
    };
}

RsgString rsg_string_from_i32(int32_t value) {
    return rsg_string_from_format("%d", value);
}

RsgString rsg_string_from_u32(uint32_t value) {
    return rsg_string_from_format("%u", value);
}

RsgString rsg_string_from_i64(int64_t value) {
    return rsg_string_from_format("%lld", (long long)value);
}

RsgString rsg_string_from_u64(uint64_t value) {
    return rsg_string_from_format("%llu", (unsigned long long)value);
}

RsgString rsg_string_from_f32(float value) {
    return rsg_string_from_format("%g", (double)value);
}

RsgString rsg_string_from_f64(double value) {
    return rsg_string_from_format("%g", value);
}

RsgString rsg_string_from_bool(bool value) {
    return rsg_string_literal(value ? "true" : "false");
}

RsgString rsg_string_from_char(char value) {
    char buffer[2] = {value, '\0'};
    return rsg_string_new(buffer, 1);
}

// String builder implementation.

void rsg_string_builder_init(RsgStringBuilder *builder) {
    builder->capacity = 64;
    builder->length = 0;
    builder->buffer = checked_malloc(builder->capacity);
}

void rsg_string_builder_append(RsgStringBuilder *builder, const char *source, int32_t length) {
    while (builder->length + length >= builder->capacity) {
        builder->capacity *= 2;
        builder->buffer = checked_realloc(builder->buffer, builder->capacity);
    }
    memcpy(builder->buffer + builder->length, source, length);
    builder->length += length;
}

void rsg_string_builder_append_string(RsgStringBuilder *builder, RsgString source) {
    rsg_string_builder_append(builder, source.data, source.length);
}

RsgString rsg_string_builder_finish(RsgStringBuilder *builder) {
    RsgString result = rsg_string_new(builder->buffer, builder->length);
    free(builder->buffer);
    builder->buffer = NULL;
    builder->length = builder->capacity = 0;
    return result;
}

bool rsg_string_equal(RsgString left, RsgString right) {
    if (left.length != right.length) {
        return false;
    }
    return memcmp(left.data, right.data, left.length) == 0;
}

void rsg_assert(bool condition, const char *message, const char *file, int32_t line) {
    if (!condition) {
        if (message != NULL) {
            fprintf(stderr, "assertion failed at %s:%d: %s\n", file, line, message);
        } else {
            fprintf(stderr, "assertion failed at %s:%d\n", file, line);
        }
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        exit(1);
    }
}

// Typed I/O - print values to stdout without a trailing newline.

void rsg_print_string(RsgString source) {
    fwrite(source.data, 1, source.length, stdout);
}

void rsg_print_i32(int32_t value) {
    printf("%d", value);
}

void rsg_print_u32(uint32_t value) {
    printf("%u", value);
}

void rsg_print_f64(double value) {
    printf("%g", value);
}

void rsg_print_bool(bool value) {
    printf("%s", value ? "true" : "false");
}
