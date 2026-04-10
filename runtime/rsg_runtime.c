#include "rsg_runtime.h"

#include <setjmp.h>

// Checked alloc helpers - abort on OOM.

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

// Str constructors and conversions.
//
// NOTE: Str data is allocated outside the GC heap (via checked_malloc)
// and managed by ref counting.  The tracing GC handles struct/value
// allocs made through rsg_heap_alloc().

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

// Str builder implementation.

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

void rsg_assert(bool cond, const char *msg, const char *file, int32_t line) {
    if (!cond) {
        if (msg != NULL) {
            rsg_panic(msg);
        } else {
            // Build "assertion failed at file:line"
            char buf[512];
            snprintf(buf, sizeof(buf), "assertion failed at %s:%d", file, line);
            rsg_panic(buf);
        }
    }
}

// Typed I/O - print values to stdout without a trailing newline.

void rsg_print_str(RsgStr src) {
    fwrite(src.data, 1, src.len, stdout);
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

void rsg_print_char(char value) {
    putchar(value);
}

// Typed I/O — print values to stdout with a trailing newline.

void rsg_println_str(RsgStr src) {
    fwrite(src.data, 1, src.len, stdout);
    putchar('\n');
}

void rsg_println_i32(int32_t value) {
    printf("%d\n", value);
}

void rsg_println_u32(uint32_t value) {
    printf("%u\n", value);
}

void rsg_println_f64(double value) {
    printf("%g\n", value);
}

void rsg_println_bool(bool value) {
    printf("%s\n", value ? "true" : "false");
}

void rsg_println_char(char value) {
    printf("%c\n", value);
}

// ── Tracing garbage collector ──────────────────────────────────────────
//
// Conservative mark-and-sweep.  Every rsg_heap_alloc alloc is
// prepended with an RsgGcObject header and tracked in a sorted ptr
// array for O(log n) lookup.  Collection scans the C stack, register
// spill area, and user-registered roots for values that look like
// ptrs into tracked allocs, marks them reachable, then frees
// everything else.
//
// Limitations (inherent to conservative collection):
//   - An integer that coincidentally looks like a heap address will pin
//     the target object.  This may cause "phantom retention" in long-
//     running programs.  A future precise mode can eliminate this.
//   - Single-threaded only.  Resurg has no concurrency primitives yet;
//     multi-threaded use requires external synchronization.

typedef struct RsgGcObject {
    size_t size; // user-visible alloc size (aligned)
    bool marked;
} RsgGcObject;

// Align every alloc so that interior-ptr scanning covers all
// ptr-aligned slots without truncation.
#define GC_ALIGN_UP(n) (((n) + sizeof(uintptr_t) - 1) & ~(sizeof(uintptr_t) - 1))

#define RSG_GC_INITIAL_THRESHOLD 256

// ── sorted object array for O(log n) lookup ──

static RsgGcObject **g_gc_objects; // sorted by user-data address (ascending)
static size_t g_gc_object_count;
static size_t g_gc_object_capacity;
static size_t g_gc_threshold = RSG_GC_INITIAL_THRESHOLD;
static uintptr_t g_gc_stack_bottom;

/** Return user-visible data ptr for a tracked header. */
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
static size_t g_gc_worklist_len;
static size_t g_gc_worklist_capacity;

static void gc_worklist_push(RsgGcObject *object) {
    if (g_gc_worklist_len >= g_gc_worklist_capacity) {
        g_gc_worklist_capacity = g_gc_worklist_capacity == 0 ? 64 : g_gc_worklist_capacity * 2;
        g_gc_worklist = (RsgGcObject **)checked_realloc(
            (void *)g_gc_worklist, g_gc_worklist_capacity * sizeof(RsgGcObject *));
    }
    g_gc_worklist[g_gc_worklist_len++] = object;
}

/** Mark @p value if it looks like a ptr to a tracked alloc. */
static void gc_mark_value(uintptr_t value) {
    RsgGcObject *object = gc_find_object(value);
    if (object == NULL || object->marked) {
        return;
    }
    object->marked = true;
    gc_worklist_push(object);
}

/** Drain the worklist, scanning each marked object for interior ptrs. */
static void gc_trace_worklist(void) {
    while (g_gc_worklist_len > 0) {
        RsgGcObject *object = g_gc_worklist[--g_gc_worklist_len];
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

    // Scan the C stack from current pos to the recorded bottom.
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

    for (uintptr_t *ptr = scan_start; ptr < scan_end; ptr++) {
        gc_mark_value(*ptr);
    }

    // Scan user-registered roots (global/static ptr slots).
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

// ── pub GC API ──

void rsg_gc_init(void *stack_bottom) {
    g_gc_stack_bottom = (uintptr_t)stack_bottom;
}

void rsg_gc_collect(void) {
    gc_mark_roots();
    gc_sweep();

    // Free the worklist — it is only needed during collection.
    free((void *)g_gc_worklist);
    g_gc_worklist = NULL;
    g_gc_worklist_len = 0;
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

// ── Slice runtime ─────────────────────────────────────────────────────

RsgSlice rsg_slice_new(const void *src, int32_t count, size_t elem_size) {
    size_t total = (size_t)count * elem_size;
    void *data = rsg_heap_alloc(total);
    if (src != NULL && count > 0) {
        memcpy(data, src, total);
    }
    return (RsgSlice){.data = data, .len = count};
}

RsgSlice rsg_slice_sub(RsgSlice slice, int32_t start, int32_t end, size_t elem_size) {
    return (RsgSlice){
        .data = (char *)slice.data + (size_t)start * elem_size,
        .len = end - start,
    };
}

RsgSlice rsg_slice_from_array(const void *array_data, int32_t count, size_t elem_size) {
    return rsg_slice_new(array_data, count, elem_size);
}

RsgSlice rsg_slice_concat(RsgSlice a, RsgSlice b, size_t elem_size) {
    int32_t total = a.len + b.len;
    void *data = rsg_heap_alloc((size_t)total * elem_size);
    if (a.len > 0) {
        memcpy(data, a.data, (size_t)a.len * elem_size);
    }
    if (b.len > 0) {
        memcpy((char *)data + (size_t)a.len * elem_size, b.data, (size_t)b.len * elem_size);
    }
    return (RsgSlice){.data = data, .len = total};
}

// ── Panic / Recover ───────────────────────────────────────────────────

static RsgPanicFrame *g_rsg_panic_stack = NULL;
static char *g_rsg_panic_msg = NULL;
static bool g_rsg_panicking = false;

void rsg_panic_push(RsgPanicFrame *frame) {
    frame->prev = g_rsg_panic_stack;
    g_rsg_panic_stack = frame;
}

void rsg_panic_pop(void) {
    if (g_rsg_panic_stack != NULL) {
        g_rsg_panic_stack = g_rsg_panic_stack->prev;
    }
}

void rsg_panic(const char *msg) {
    free(g_rsg_panic_msg);
    g_rsg_panic_msg = NULL;
    if (msg != NULL) {
        size_t len = strlen(msg);
        g_rsg_panic_msg = (char *)malloc(len + 1);
        if (g_rsg_panic_msg != NULL) {
            memcpy(g_rsg_panic_msg, msg, len + 1);
        }
    }
    g_rsg_panicking = true;

    if (g_rsg_panic_stack != NULL) {
        longjmp(g_rsg_panic_stack->env, 1);
    }

    // No recovery frame — print and exit
    fprintf(stderr, "panic: %s\n", msg != NULL ? msg : "(nil)");
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    exit(1);
}

const char *rsg_recover(void) {
    if (!g_rsg_panicking) {
        return NULL;
    }
    g_rsg_panicking = false;
    return g_rsg_panic_msg;
}

bool rsg_is_panicking(void) {
    return g_rsg_panicking;
}

void rsg_repanic(void) {
    if (g_rsg_panic_stack != NULL) {
        longjmp(g_rsg_panic_stack->env, 1);
    }
    fprintf(stderr, "panic: %s\n", g_rsg_panic_msg != NULL ? g_rsg_panic_msg : "(nil)");
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    exit(1);
}
