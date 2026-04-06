#include "rsg_runtime.h"

#include <setjmp.h>

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
// NOTE: String data is allocated outside the GC heap (via checked_malloc)
// and managed by reference counting.  The tracing GC handles struct/value
// allocations made through rsg_heap_alloc().

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

// ── Tracing garbage collector ──────────────────────────────────────────
//
// Conservative mark-and-sweep.  Every rsg_heap_alloc allocation is
// prepended with an RsgGcObject header and tracked in a sorted pointer
// array for O(log n) lookup.  Collection scans the C stack, register
// spill area, and user-registered roots for values that look like
// pointers into tracked allocations, marks them reachable, then frees
// everything else.
//
// Limitations (inherent to conservative collection):
//   - An integer that coincidentally looks like a heap address will pin
//     the target object.  This may cause "phantom retention" in long-
//     running programs.  A future precise mode can eliminate this.
//   - Single-threaded only.  Resurg has no concurrency primitives yet;
//     multi-threaded use requires external synchronization.

typedef struct RsgGcObject {
    size_t size; // user-visible allocation size (aligned)
    bool marked;
} RsgGcObject;

// Align every allocation so that interior-pointer scanning covers all
// pointer-aligned slots without truncation.
#define GC_ALIGN_UP(n) (((n) + sizeof(uintptr_t) - 1) & ~(sizeof(uintptr_t) - 1))

#define RSG_GC_INITIAL_THRESHOLD 256

// ── sorted object array for O(log n) lookup ──

static RsgGcObject **g_gc_objects; // sorted by user-data address (ascending)
static size_t g_gc_object_count;
static size_t g_gc_object_capacity;
static size_t g_gc_threshold = RSG_GC_INITIAL_THRESHOLD;
static uintptr_t g_gc_stack_bottom;

/** Return user-visible data pointer for a tracked header. */
static inline void *gc_object_data(RsgGcObject *object) {
    return (void *)(object + 1);
}

/** Return the user-data address as uintptr_t. */
static inline uintptr_t gc_object_start(RsgGcObject *object) {
    return (uintptr_t)gc_object_data(object);
}

/**
 * Binary-search for the object whose user-data region contains @p value.
 * Returns NULL when no match.
 */
static RsgGcObject *gc_find_object(uintptr_t value) {
    if (g_gc_object_count == 0) {
        return NULL;
    }
    // Find the rightmost object whose start <= value.
    size_t low = 0;
    size_t high = g_gc_object_count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (gc_object_start(g_gc_objects[mid]) <= value) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    if (low == 0) {
        return NULL;
    }
    RsgGcObject *candidate = g_gc_objects[low - 1];
    uintptr_t start = gc_object_start(candidate);
    if (value >= start && value < start + candidate->size) {
        return candidate;
    }
    return NULL;
}

/** Insert @p object into the sorted array, maintaining ascending order. */
static void gc_insert_object(RsgGcObject *object) {
    if (g_gc_object_count >= g_gc_object_capacity) {
        g_gc_object_capacity = g_gc_object_capacity == 0 ? 256 : g_gc_object_capacity * 2;
        g_gc_objects = (RsgGcObject **)checked_realloc(
            (void *)g_gc_objects, g_gc_object_capacity * sizeof(RsgGcObject *));
    }
    uintptr_t key = gc_object_start(object);
    // Binary search for insertion point.
    size_t low = 0;
    size_t high = g_gc_object_count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (gc_object_start(g_gc_objects[mid]) < key) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    memmove((void *)&g_gc_objects[low + 1], (const void *)&g_gc_objects[low],
            (g_gc_object_count - low) * sizeof(RsgGcObject *));
    g_gc_objects[low] = object;
    g_gc_object_count++;
}

// ── user-registered roots ──

static void ***g_gc_roots;
static size_t g_gc_root_count;
static size_t g_gc_root_capacity;

// ── worklist for iterative marking ──

static RsgGcObject **g_gc_worklist;
static size_t g_gc_worklist_length;
static size_t g_gc_worklist_capacity;

static void gc_worklist_push(RsgGcObject *object) {
    if (g_gc_worklist_length >= g_gc_worklist_capacity) {
        g_gc_worklist_capacity = g_gc_worklist_capacity == 0 ? 64 : g_gc_worklist_capacity * 2;
        g_gc_worklist = (RsgGcObject **)checked_realloc(
            (void *)g_gc_worklist, g_gc_worklist_capacity * sizeof(RsgGcObject *));
    }
    g_gc_worklist[g_gc_worklist_length++] = object;
}

/** Mark @p value if it looks like a pointer to a tracked allocation. */
static void gc_mark_value(uintptr_t value) {
    RsgGcObject *object = gc_find_object(value);
    if (object == NULL || object->marked) {
        return;
    }
    object->marked = true;
    gc_worklist_push(object);
}

/** Drain the worklist, scanning each marked object for interior pointers. */
static void gc_trace_worklist(void) {
    while (g_gc_worklist_length > 0) {
        RsgGcObject *object = g_gc_worklist[--g_gc_worklist_length];
        uintptr_t *scan = (uintptr_t *)gc_object_data(object);
        size_t word_count = object->size / sizeof(uintptr_t);
        for (size_t i = 0; i < word_count; i++) {
            gc_mark_value(scan[i]);
        }
    }
}

// ── mark phase ──

static void gc_mark_roots(void) {
    // Flush registers into a jmp_buf so they are scannable on the stack.
    jmp_buf registers;
    // NOLINTNEXTLINE(cert-err52-cpp)
    (void)setjmp(registers);

    uintptr_t *register_scan = (uintptr_t *)&registers;
    // NOLINTNEXTLINE(bugprone-sizeof-expression)
    size_t register_word_count = sizeof(registers) / (sizeof(uintptr_t));
    for (size_t i = 0; i < register_word_count; i++) {
        gc_mark_value(register_scan[i]);
    }

    // Scan the C stack from current position to the recorded bottom.
    volatile uintptr_t stack_top_anchor;
    uintptr_t stack_top = (uintptr_t)&stack_top_anchor;

    uintptr_t *scan_start;
    uintptr_t *scan_end;
    if (stack_top < g_gc_stack_bottom) {
        scan_start = (uintptr_t *)stack_top;
        scan_end = (uintptr_t *)g_gc_stack_bottom;
    } else {
        scan_start = (uintptr_t *)g_gc_stack_bottom;
        scan_end = (uintptr_t *)stack_top;
    }

    for (uintptr_t *pointer = scan_start; pointer < scan_end; pointer++) {
        gc_mark_value(*pointer);
    }

    // Scan user-registered roots (global/static pointer slots).
    for (size_t i = 0; i < g_gc_root_count; i++) {
        void *root_value = *g_gc_roots[i];
        if (root_value != NULL) {
            gc_mark_value((uintptr_t)root_value);
        }
    }

    gc_trace_worklist();
}

// ── sweep phase ──

static void gc_sweep(void) {
    size_t write = 0;
    for (size_t read = 0; read < g_gc_object_count; read++) {
        RsgGcObject *object = g_gc_objects[read];
        if (object->marked) {
            object->marked = false;
            g_gc_objects[write++] = object;
        } else {
            free(object);
        }
    }
    g_gc_object_count = write;
}

// ── public GC API ──

void rsg_gc_init(void *stack_bottom) {
    g_gc_stack_bottom = (uintptr_t)stack_bottom;
}

void rsg_gc_collect(void) {
    gc_mark_roots();
    gc_sweep();

    // Free the worklist — it is only needed during collection.
    free((void *)g_gc_worklist);
    g_gc_worklist = NULL;
    g_gc_worklist_length = 0;
    g_gc_worklist_capacity = 0;

    g_gc_threshold = g_gc_object_count < RSG_GC_INITIAL_THRESHOLD / 2 ? RSG_GC_INITIAL_THRESHOLD
                                                                      : g_gc_object_count * 2;
}

void rsg_gc_add_root(void **root) {
    if (g_gc_root_count >= g_gc_root_capacity) {
        g_gc_root_capacity = g_gc_root_capacity == 0 ? 16 : g_gc_root_capacity * 2;
        g_gc_roots =
            (void ***)checked_realloc((void *)g_gc_roots, g_gc_root_capacity * sizeof(*g_gc_roots));
    }
    g_gc_roots[g_gc_root_count++] = root;
}

void rsg_gc_remove_root(void **root) {
    for (size_t i = 0; i < g_gc_root_count; i++) {
        if (g_gc_roots[i] == root) {
            g_gc_roots[i] = g_gc_roots[--g_gc_root_count];
            return;
        }
    }
}

void *rsg_heap_alloc(size_t size) {
    if (g_gc_object_count >= g_gc_threshold) {
        rsg_gc_collect();
    }

    size_t aligned_size = GC_ALIGN_UP(size);
    RsgGcObject *object = checked_malloc(sizeof(RsgGcObject) + aligned_size);
    object->size = aligned_size;
    object->marked = false;

    gc_insert_object(object);

    return gc_object_data(object);
}
