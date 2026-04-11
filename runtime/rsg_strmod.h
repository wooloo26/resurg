#ifndef RSG_STRMOD_H
#define RSG_STRMOD_H

/**
 * @file rsg_strmod.h
 * @brief std/str module — free string utility functions.
 *
 * Extension methods on str live in rsg_str.h and are declared in
 * std/builtin.rsg.  This module provides free functions for algorithms
 * that don't have a natural receiver.
 */

#include <stdint.h>

#include "rsg_slice.h"
#include "rsg_str.h"

/** Join a slice of strings with a separator. */
RsgStr rsg_str_mod_join(RsgSlice parts, RsgStr sep);
/** Format an integer in the given base (2, 8, 10, 16). */
RsgStr rsg_str_mod_format_int(int64_t val, int32_t base);

#endif // RSG_STRMOD_H
