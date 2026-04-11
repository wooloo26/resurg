#ifndef RSG_STR_H
#define RSG_STR_H

/**
 * @file rsg_str.h
 * @brief Immutable ref-counted strs and str builder.
 */

#include <stdbool.h>
#include <stdint.h>

/**
 * Immutable, ref-counted str.  ref_count == -1 marks a
 * static str lit (no heap alloc).
 *
 * @note Str data uses its own allocator (not the GC heap).  The
 *       tracing GC manages struct/value allocs made via
 *       rsg_heap_alloc(); str data is refcounted separately.
 */
typedef struct {
    const char *data;
    int32_t len;
    int32_t ref_count;
} RsgStr;

/** Wrap a C str lit (zero-copy, static refcount). */
RsgStr rsg_str_lit(const char *src);
/** Copy @p len bytes from @p src into a new heap-allocated str. */
RsgStr rsg_str_new(const char *src, int32_t len);
/** Concatenate two strs into a new heap-allocated str. */
RsgStr rsg_str_concat(RsgStr a, RsgStr b);
/** Return an empty static str. */
RsgStr rsg_str_empty(void);

/** Convert an i32 to its decimal str representation. */
RsgStr rsg_str_from_i32(int32_t value);
/** Convert a u32 to its decimal str representation. */
RsgStr rsg_str_from_u32(uint32_t value);
/** Convert an i64 to its decimal str representation. */
RsgStr rsg_str_from_i64(int64_t value);
/** Convert a u64 to its decimal str representation. */
RsgStr rsg_str_from_u64(uint64_t value);
/** Convert an f32 to its shortest str representation. */
RsgStr rsg_str_from_f32(float value);
/** Convert an f64 to its shortest str representation. */
RsgStr rsg_str_from_f64(double value);
/** Convert a bool to "true" or "false". */
RsgStr rsg_str_from_bool(bool value);
/** Convert a char to a single-character str. */
RsgStr rsg_str_from_char(char value);

/**
 * Str builder - growable byte buf for assembling interpolated strs
 * with many parts.
 */
typedef struct {
    char *buf;
    int32_t len;
    int32_t capacity;
} RsgStrBuilder;

/** Initialise @p builder with a default capacity. */
void rsg_str_builder_init(RsgStrBuilder *builder);
/** Append @p len raw bytes from @p src. */
void rsg_str_builder_append(RsgStrBuilder *builder, const char *src, int32_t len);
/** Append an RsgStr. */
void rsg_str_builder_append_str(RsgStrBuilder *builder, RsgStr src);
/**
 * Finalise the builder, return the assembled RsgStr, and free internal
 * storage.
 */
RsgStr rsg_str_builder_finish(RsgStrBuilder *builder);

/** Return true if @p a and @p b have identical content. */
bool rsg_str_equal(RsgStr a, RsgStr b);

// ── Extension methods (called by generated code via decl fn) ───

/** Return true when @p s contains @p needle as a substring. */
bool rsg_str_contains(RsgStr s, RsgStr needle);
/** Return true when @p s starts with @p prefix. */
bool rsg_str_starts_with(RsgStr s, RsgStr prefix);
/** Return true when @p s ends with @p suffix. */
bool rsg_str_ends_with(RsgStr s, RsgStr suffix);
/** Strip leading and trailing ASCII whitespace from @p s. */
RsgStr rsg_str_trim(RsgStr s);
/** Strip leading ASCII whitespace from @p s. */
RsgStr rsg_str_trim_start(RsgStr s);
/** Strip trailing ASCII whitespace from @p s. */
RsgStr rsg_str_trim_end(RsgStr s);
/** Repeat @p s exactly @p n times. */
RsgStr rsg_str_repeat(RsgStr s, int32_t n);
/** Convert all ASCII letters to uppercase. */
RsgStr rsg_str_to_upper(RsgStr s);
/** Convert all ASCII letters to lowercase. */
RsgStr rsg_str_to_lower(RsgStr s);

#endif // RSG_STR_H
