#ifndef RG_RUNTIME_H
#define RG_RUNTIME_H

/**
 * @file runtime.h
 * @brief Resurg runtime library — linked into every compiled program.
 *
 * Provides: immutable reference-counted strings, string interpolation
 * helpers, assert, and typed I/O functions.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Immutable, reference-counted string.  reference_count == -1 marks a
 * static string literal (no heap allocation).
 */
typedef struct {
    const char *data;
    int32_t length;
    int32_t reference_count;
} RgString;

/** Wrap a C string literal (zero-copy, static refcount). */
RgString rg_string_literal(const char *source);
/** Copy @p length bytes from @p source into a new heap-allocated string. */
RgString rg_string_new(const char *source, int32_t length);
/** Concatenate two strings into a new heap-allocated string. */
RgString rg_string_concat(RgString a, RgString b);
/** Return an empty static string. */
RgString rg_string_empty(void);

/** Convert an i32 to its decimal string representation. */
RgString rg_string_from_i32(int32_t value);
/** Convert a u32 to its decimal string representation. */
RgString rg_string_from_u32(uint32_t value);
/** Convert an f64 to its shortest string representation. */
RgString rg_string_from_f64(double value);
/** Convert a bool to "true" or "false". */
RgString rg_string_from_bool(bool value);

/**
 * String builder — growable byte buffer for assembling interpolated strings
 * with many parts.
 */
typedef struct {
    char *buffer;
    int32_t length;
    int32_t capacity;
} RgStringBuilder;

/** Initialise @p builder with a default capacity. */
void rg_string_builder_init(RgStringBuilder *builder);
/** Append @p length raw bytes from @p source. */
void rg_string_builder_append(RgStringBuilder *builder, const char *source, int32_t length);
/** Append an RgString. */
void rg_string_builder_append_string(RgStringBuilder *builder, RgString source);
/**
 * Finalise the builder, return the assembled RgString, and free internal
 * storage.
 */
RgString rg_string_builder_finish(RgStringBuilder *builder);

/** Return true if @p a and @p b have identical content. */
bool rg_string_equal(RgString a, RgString b);

/**
 * Abort with @p message if @p condition is false.  @p file and @p line are
 * embedded by the code generator for diagnostics.
 */
void rg_assert(bool condition, const char *message, const char *file, int32_t line);

/** Print an RgString to stdout (no trailing newline). */
void rg_print_string(RgString source);
void rg_print_i32(int32_t value);
void rg_print_u32(uint32_t value);
void rg_print_f64(double value);
void rg_print_bool(bool value);

#endif // RG_RUNTIME_H
