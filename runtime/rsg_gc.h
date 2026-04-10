#ifndef RSG_GC_H
#define RSG_GC_H

/**
 * @file rsg_gc.h
 * @brief Conservative mark-and-sweep garbage collector.
 */

#include <stddef.h>

/** Allocate @p size bytes on the GC-managed heap; abort on OOM. */
void *rsg_heap_alloc(size_t size);

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

#endif // RSG_GC_H
