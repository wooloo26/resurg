#include "runtime.h"

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

/** Build an RgString via printf-style formatting. */
static RgString rg_string_from_format(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    int32_t length = vsnprintf(NULL, 0, format, arguments);
    va_end(arguments);
    char *buffer = checked_malloc(length + 1);
    va_start(arguments, format);
    vsnprintf(buffer, length + 1, format, arguments);
    va_end(arguments);
    return (RgString){
        .data = buffer,
        .length = length,
        .reference_count = 1,
    };
}

// String constructors and conversions.

RgString rg_string_literal(const char *source) {
    return (RgString){
        .data = source,
        .length = (int32_t)strlen(source),
        .reference_count = -1, // static
    };
}

RgString rg_string_new(const char *source, int32_t length) {
    char *buffer = checked_malloc(length + 1);
    memcpy(buffer, source, length);
    buffer[length] = '\0';
    return (RgString){
        .data = buffer,
        .length = length,
        .reference_count = 1,
    };
}

RgString rg_string_empty(void) {
    return rg_string_literal("");
}

RgString rg_string_concat(RgString left, RgString right) {
    int32_t length = left.length + right.length;
    char *buffer = checked_malloc(length + 1);
    memcpy(buffer, left.data, left.length);
    memcpy(buffer + left.length, right.data, right.length);
    buffer[length] = '\0';
    return (RgString){
        .data = buffer,
        .length = length,
        .reference_count = 1,
    };
}

RgString rg_string_from_i32(int32_t value) {
    return rg_string_from_format("%d", value);
}

RgString rg_string_from_u32(uint32_t value) {
    return rg_string_from_format("%u", value);
}

RgString rg_string_from_f64(double value) {
    return rg_string_from_format("%g", value);
}

RgString rg_string_from_bool(bool value) {
    return rg_string_literal(value ? "true" : "false");
}

// String builder implementation.

void rg_string_builder_init(RgStringBuilder *builder) {
    builder->capacity = 64;
    builder->length = 0;
    builder->buffer = checked_malloc(builder->capacity);
}

void rg_string_builder_append(RgStringBuilder *builder, const char *source, int32_t length) {
    while (builder->length + length >= builder->capacity) {
        builder->capacity *= 2;
        builder->buffer = checked_realloc(builder->buffer, builder->capacity);
    }
    memcpy(builder->buffer + builder->length, source, length);
    builder->length += length;
}

void rg_string_builder_append_string(RgStringBuilder *builder, RgString source) {
    rg_string_builder_append(builder, source.data, source.length);
}

RgString rg_string_builder_finish(RgStringBuilder *builder) {
    RgString result = rg_string_new(builder->buffer, builder->length);
    free(builder->buffer);
    builder->buffer = NULL;
    builder->length = builder->capacity = 0;
    return result;
}

bool rg_string_equal(RgString left, RgString right) {
    if (left.length != right.length) {
        return false;
    }
    return memcmp(left.data, right.data, left.length) == 0;
}

void rg_assert(bool condition, const char *message, const char *file, int32_t line) {
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

void rg_print_string(RgString source) {
    fwrite(source.data, 1, source.length, stdout);
}

void rg_print_i32(int32_t value) {
    printf("%d", value);
}

void rg_print_u32(uint32_t value) {
    printf("%u", value);
}

void rg_print_f64(double value) {
    printf("%g", value);
}

void rg_print_bool(bool value) {
    printf("%s", value ? "true" : "false");
}
