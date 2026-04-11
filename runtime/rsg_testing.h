#ifndef RSG_TESTING_H
#define RSG_TESTING_H

/**
 * @file rsg_testing.h
 * @brief std/testing module — test assertion helpers.
 */

#include <stdbool.h>
#include <stdint.h>

#include "rsg_str.h"

/** Assert equality of two i32 values with descriptive failure. */
void rsg_testing_assert_eq_i32(int32_t actual, int32_t expected);
/** Assert equality of two strings with descriptive failure. */
void rsg_testing_assert_eq_str(RsgStr actual, RsgStr expected);
/** Assert equality of two f64 values. */
void rsg_testing_assert_eq_f64(double actual, double expected);
/** Assert equality of two bools. */
void rsg_testing_assert_eq_bool(bool actual, bool expected);

/** Assert inequality of two i32 values. */
void rsg_testing_assert_ne_i32(int32_t actual, int32_t expected);
/** Assert inequality of two strings. */
void rsg_testing_assert_ne_str(RsgStr actual, RsgStr expected);

#endif // RSG_TESTING_H
