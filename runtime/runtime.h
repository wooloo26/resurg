#ifndef RG_RUNTIME_H
#define RG_RUNTIME_H

// ------------------------------------------------------------------------
// Resurg runtime library — linked into compiled programs.
// Provides: assert, string type, string interpolation, I/O.
// ------------------------------------------------------------------------
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ------------------------------------------------------------------------
// String type — immutable, reference-counted, heap-allocated.
// ------------------------------------------------------------------------
typedef struct {
    const char *data;
    int32_t len;
    int32_t refcount; // -1 = static (string literals)
} RgStr;

// Wrap a C string literal (no copy, static refcount).
RgStr rg_str_lit(const char *s);
// Copy len bytes into a new heap-allocated string.
RgStr rg_str_new(const char *s, int32_t len);
// Concatenate two strings into a new heap-allocated string.
RgStr rg_str_concat(RgStr a, RgStr b);
// Return an empty string.
RgStr rg_str_empty(void);

// Convert an i32 to its string representation.
RgStr rg_str_from_i32(int32_t v);
// Convert a u32 to its string representation.
RgStr rg_str_from_u32(uint32_t v);
// Convert an f64 to its string representation.
RgStr rg_str_from_f64(double v);
// Convert a bool to "true" or "false".
RgStr rg_str_from_bool(bool v);

// String builder — for interpolation with many parts
typedef struct {
    char *buf;
    int32_t len;
    int32_t cap;
} RgStrBuilder;

// Initialize a string builder.
void rg_sb_init(RgStrBuilder *sb);
// Append raw bytes to the builder.
void rg_sb_append(RgStrBuilder *sb, const char *s, int32_t len);
// Append an RgStr to the builder.
void rg_sb_append_str(RgStrBuilder *sb, RgStr s);
// Finalize the builder and return the built string.
RgStr rg_sb_finish(RgStrBuilder *sb);

// Return true if two strings have equal content.
bool rg_str_eq(RgStr a, RgStr b);

// ------------------------------------------------------------------------
// Assert
// ------------------------------------------------------------------------
// Abort with a message if cond is false.
void rg_assert(bool cond, const char *msg, const char *file, int32_t line);

// ------------------------------------------------------------------------
// I/O
// ------------------------------------------------------------------------
void rg_print_str(RgStr s);
void rg_print_i32(int32_t v);
void rg_print_u32(uint32_t v);
void rg_print_f64(double v);
void rg_print_bool(bool v);

#endif // RG_RUNTIME_H
