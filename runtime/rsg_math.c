#include "rsg_math.h"

#include <math.h>

// ── Absolute value / min / max / clamp ─────────────────────────────────

int32_t rsg_math_abs_i32(int32_t x) {
    return x < 0 ? -x : x;
}

double rsg_math_abs_f64(double x) {
    return fabs(x);
}

int32_t rsg_math_min_i32(int32_t a, int32_t b) {
    return a < b ? a : b;
}

int32_t rsg_math_max_i32(int32_t a, int32_t b) {
    return a > b ? a : b;
}

double rsg_math_min_f64(double a, double b) {
    return fmin(a, b);
}

double rsg_math_max_f64(double a, double b) {
    return fmax(a, b);
}

int32_t rsg_math_clamp_i32(int32_t x, int32_t lo, int32_t hi) {
    if (x < lo) {
        return lo;
    }
    if (x > hi) {
        return hi;
    }
    return x;
}

double rsg_math_clamp_f64(double x, double lo, double hi) {
    return fmin(fmax(x, lo), hi);
}

// ── Rounding / roots / power ───────────────────────────────────────────

double rsg_math_floor(double x) {
    return floor(x);
}

double rsg_math_ceil(double x) {
    return ceil(x);
}

double rsg_math_round(double x) {
    return round(x);
}

double rsg_math_trunc(double x) {
    return trunc(x);
}

double rsg_math_sqrt(double x) {
    return sqrt(x);
}

double rsg_math_cbrt(double x) {
    return cbrt(x);
}

double rsg_math_pow(double base, double exp) {
    return pow(base, exp);
}

// ── Logarithms ─────────────────────────────────────────────────────────

double rsg_math_ln(double x) {
    return log(x);
}

double rsg_math_log2(double x) {
    return log2(x);
}

double rsg_math_log10(double x) {
    return log10(x);
}

// ── Trigonometry ───────────────────────────────────────────────────────

double rsg_math_sin(double x) {
    return sin(x);
}

double rsg_math_cos(double x) {
    return cos(x);
}

double rsg_math_tan(double x) {
    return tan(x);
}

double rsg_math_asin(double x) {
    return asin(x);
}

double rsg_math_acos(double x) {
    return acos(x);
}

double rsg_math_atan(double x) {
    return atan(x);
}

double rsg_math_atan2(double y, double x) {
    return atan2(y, x);
}

// ── Classification ─────────────────────────────────────────────────────

bool rsg_math_is_nan(double x) {
    return isnan(x);
}

bool rsg_math_is_inf(double x) {
    return isinf(x);
}

bool rsg_math_is_finite(double x) {
    return isfinite(x);
}
