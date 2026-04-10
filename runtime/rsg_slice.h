#ifndef RSG_SLICE_H
#define RSG_SLICE_H

/**
 * @file rsg_slice.h
 * @brief Generic slice runtime — fat ptr (data + len).
 */

#include <stddef.h>
#include <stdint.h>

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
/** Concatenate two slices into a new GC-allocated slice. */
RsgSlice rsg_slice_concat(RsgSlice a, RsgSlice b, size_t elem_size);

#endif // RSG_SLICE_H
