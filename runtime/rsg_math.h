#ifndef RSG_MATH_H
#define RSG_MATH_H

/**
 * @file rsg_math.h
 * @brief std/math module — mathematical constants and functions.
 *
 * All functions are prefixed rsg_math_ to match the module mangling.
 */

#include <stdbool.h>
#include <stdint.h>

// ── Functions ─────────────────────────────────────────────────────────

int32_t rsg_math_abs_i32(int32_t x);
double rsg_math_abs_f64(double x);
int32_t rsg_math_min_i32(int32_t a, int32_t b);
int32_t rsg_math_max_i32(int32_t a, int32_t b);
double rsg_math_min_f64(double a, double b);
double rsg_math_max_f64(double a, double b);
int32_t rsg_math_clamp_i32(int32_t x, int32_t lo, int32_t hi);
double rsg_math_clamp_f64(double x, double lo, double hi);

double rsg_math_floor(double x);
double rsg_math_ceil(double x);
double rsg_math_round(double x);
double rsg_math_trunc(double x);
double rsg_math_sqrt(double x);
double rsg_math_cbrt(double x);
double rsg_math_pow(double base, double exp);

double rsg_math_ln(double x);
double rsg_math_log2(double x);
double rsg_math_log10(double x);

double rsg_math_sin(double x);
double rsg_math_cos(double x);
double rsg_math_tan(double x);
double rsg_math_asin(double x);
double rsg_math_acos(double x);
double rsg_math_atan(double x);
double rsg_math_atan2(double y, double x);

bool rsg_math_is_nan(double x);
bool rsg_math_is_inf(double x);
bool rsg_math_is_finite(double x);

#endif // RSG_MATH_H
