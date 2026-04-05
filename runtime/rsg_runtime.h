#ifndef RG_RUNTIME_H
#define RG_RUNTIME_H

/**
 * @file runtime.h
 * @brief Resurg runtime library - linked into every compiled program.
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
 *
 * @note Heap-allocated strings are currently never freed.  This is an
 *       intentional simplification until the tracing GC lands (v0.4.0).
 *       Short-lived test programs are unaffected.
 */
typedef struct {
    const char *data;
    int32_t length;
    int32_t reference_count;
} RsgString;

/** Wrap a C string literal (zero-copy, static refcount). */
RsgString rsg_string_literal(const char *source);
/** Copy @p length bytes from @p source into a new heap-allocated string. */
RsgString rsg_string_new(const char *source, int32_t length);
/** Concatenate two strings into a new heap-allocated string. */
RsgString rsg_string_concat(RsgString a, RsgString b);
/** Return an empty static string. */
RsgString rsg_string_empty(void);

/** Convert an i32 to its decimal string representation. */
RsgString rsg_string_from_i32(int32_t value);
/** Convert a u32 to its decimal string representation. */
RsgString rsg_string_from_u32(uint32_t value);
/** Convert an i64 to its decimal string representation. */
RsgString rsg_string_from_i64(int64_t value);
/** Convert a u64 to its decimal string representation. */
RsgString rsg_string_from_u64(uint64_t value);
/** Convert an f32 to its shortest string representation. */
RsgString rsg_string_from_f32(float value);
/** Convert an f64 to its shortest string representation. */
RsgString rsg_string_from_f64(double value);
/** Convert a bool to "true" or "false". */
RsgString rsg_string_from_bool(bool value);
/** Convert a char to a single-character string. */
RsgString rsg_string_from_char(char value);

/**
 * String builder - growable byte buffer for assembling interpolated strings
 * with many parts.
 */
typedef struct {
    char *buffer;
    int32_t length;
    int32_t capacity;
} RsgStringBuilder;

/** Initialise @p builder with a default capacity. */
void rsg_string_builder_init(RsgStringBuilder *builder);
/** Append @p length raw bytes from @p source. */
void rsg_string_builder_append(RsgStringBuilder *builder, const char *source, int32_t length);
/** Append an RsgString. */
void rsg_string_builder_append_string(RsgStringBuilder *builder, RsgString source);
/**
 * Finalise the builder, return the assembled RsgString, and free internal
 * storage.
 */
RsgString rsg_string_builder_finish(RsgStringBuilder *builder);

/** Return true if @p a and @p b have identical content. */
bool rsg_string_equal(RsgString a, RsgString b);

/**
 * Abort with @p message if @p condition is false.  @p file and @p line are
 * embedded by the code generator for diagnostics.
 */
void rsg_assert(bool condition, const char *message, const char *file, int32_t line);

/** Print an RsgString to stdout (no trailing newline). */
void rsg_print_string(RsgString source);
void rsg_print_i32(int32_t value);
void rsg_print_u32(uint32_t value);
void rsg_print_f64(double value);
void rsg_print_bool(bool value);

/** Allocate @p size bytes on the heap; abort on OOM. */
void *rsg_heap_alloc(size_t size);

#endif // RG_RUNTIME_H
