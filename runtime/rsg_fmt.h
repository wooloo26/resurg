#ifndef RSG_FMT_H
#define RSG_FMT_H

/**
 * @file rsg_fmt.h
 * @brief std/fmt module — string formatting utilities.
 */

#include <stdint.h>

#include "rsg_str.h"

/** Format a float with fixed decimal places. */
RsgStr rsg_fmt_fixed(double value, int32_t decimals);
/** Hex representation of an integer. */
RsgStr rsg_fmt_hex(int64_t value);
/** Binary representation of an integer. */
RsgStr rsg_fmt_bin(int64_t value);
/** Octal representation of an integer. */
RsgStr rsg_fmt_oct(int64_t value);
/** Left-pad to minimum width. */
RsgStr rsg_fmt_pad_left(RsgStr s, int32_t width, char fill);
/** Right-pad to minimum width. */
RsgStr rsg_fmt_pad_right(RsgStr s, int32_t width, char fill);

#endif // RSG_FMT_H
