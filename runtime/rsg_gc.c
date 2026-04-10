#include "rsg_gc.h"

#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "rsg_internal.h"

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
