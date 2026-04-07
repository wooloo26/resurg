#ifndef RSG_RUNTIME_H
#define RSG_RUNTIME_H

/**
 * @file rsg_runtime.h
 * @brief Resurg runtime library - linked into every compiled program.
 *
 * Provides: immutable ref-counted strs, str interpolation
 * helpers, assert, and typed I/O fns.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/**
 * Abort with @p msg if @p cond is false.  @p file and @p line are
 * embedded by the code generator for diagnostics.
 */
void rsg_assert(bool cond, const char *msg, const char *file, int32_t line);

/** Print an RsgStr to stdout (no trailing newline). */
void rsg_print_str(RsgStr src);
void rsg_print_i32(int32_t value);
void rsg_print_u32(uint32_t value);
void rsg_print_f64(double value);
void rsg_print_bool(bool value);
void rsg_print_char(char value);

/** Print to stdout with a trailing newline. */
void rsg_println_str(RsgStr src);
void rsg_println_i32(int32_t value);
void rsg_println_u32(uint32_t value);
void rsg_println_f64(double value);
void rsg_println_bool(bool value);
void rsg_println_char(char value);

/** Allocate @p size bytes on the GC-managed heap; abort on OOM. */
void *rsg_heap_alloc(size_t size);

// ── Slice runtime ─────────────────────────────────────────────────────

/**
 * Generic slice header — a fat ptr (data + len).
 * The data ptr refers to GC-managed storage.
 */
typedef struct {
    void *data;
    int32_t len;
} RsgSlice;

/** Create a new slice by copying @p count elems of @p elem_size from @p src. */
RsgSlice rsg_slice_new(const void *src, int32_t count, size_t elem_size);
/** Create a sub-slice sharing backing storage (no copy). */
RsgSlice rsg_slice_sub(RsgSlice slice, int32_t start, int32_t end, size_t elem_size);
/** Create a slice from an array (copies data into GC storage). */
RsgSlice rsg_slice_from_array(const void *array_data, int32_t count, size_t elem_size);

/**
 * Initialise the tracing garbage collector.  Must be called once at the
 * start of main() with the address of a local var to mark the stack
 * bottom.
 */
void rsg_gc_init(void *stack_bottom);

/** Run a full mark-and-sweep collection cycle. */
void rsg_gc_collect(void);

/**
 * Register @p root as an additional GC root.  @p root must point to a
 * `void *` slot that holds a GC-managed ptr (or NULL).  The GC will
 * scan this slot during every collection.  Typical use: global or static
 * vars that hold heap ptrs.
 */
void rsg_gc_add_root(void **root);

/**
 * Remove a previously registered root.  After this call, the GC no longer
 * considers @p root a src of liveness.
 */
void rsg_gc_remove_root(void **root);

/**
 * Fat pointer for first-class function values (fn types).
 * @c fn is the actual function (cast at call site to the correct
 * signature with an extra leading @c void* env param).
 * @c env holds captured state (NULL for plain function references
 * and non-capturing closures).
 */
typedef struct {
    void (*fn)(void);
    void *env;
} RsgFn;

#endif // RSG_RUNTIME_H
