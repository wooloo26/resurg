#ifndef RSG_SORT_H
#define RSG_SORT_H

/**
 * @file rsg_sort.h
 * @brief std/sort module — sorting for typed slices.
 *
 * Type-specific sort functions used via decl fn in std/sort.rsg.
 */

#include <stdbool.h>
#include <stdint.h>

#include "rsg_slice.h"
#include "rsg_str.h"

/** In-place sort of an i32 slice. */
void rsg_sort_sort_i32(RsgSlice *s);
/** In-place sort of an i64 slice. */
void rsg_sort_sort_i64(RsgSlice *s);
/** In-place sort of an f64 slice. */
void rsg_sort_sort_f64(RsgSlice *s);
/** In-place sort of a str slice (lexicographic). */
void rsg_sort_sort_str(RsgSlice *s);
/** Check if an i32 slice is sorted. */
bool rsg_sort_is_sorted_i32(RsgSlice s);
/** Check if a str slice is sorted. */
bool rsg_sort_is_sorted_str(RsgSlice s);
/** Binary search in a sorted i32 slice. Returns index or -1. */
int32_t rsg_sort_binary_search_i32(RsgSlice s, int32_t target);

#endif // RSG_SORT_H
