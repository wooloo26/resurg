#include "rsg_prim.h"

#include <math.h>
#include <stdlib.h>

// ── i32 extensions ────────────────────────────────────────────────────

int32_t rsg_i32_abs(int32_t n) {
    return n < 0 ? -n : n;
}

int32_t rsg_i32_min(int32_t n, int32_t other) {
    return n < other ? n : other;
}

int32_t rsg_i32_max(int32_t n, int32_t other) {
    return n > other ? n : other;
}

int32_t rsg_i32_clamp(int32_t n, int32_t lo, int32_t hi) {
    if (n < lo) {
        return lo;
    }
    if (n > hi) {
        return hi;
    }
    return n;
}

RsgStr rsg_i32_to_str(int32_t n) {
    return rsg_str_from_i32(n);
}

double rsg_i32_to_f64(int32_t n) {
    return (double)n;
}

int32_t rsg_i32_pow(int32_t n, uint32_t exp) {
    int32_t result = 1;
    int32_t base = n;
    while (exp > 0) {
        if (exp & 1) {
            result *= base;
        }
        base *= base;
        exp >>= 1;
    }
    return result;
}

// ── i64 extensions ────────────────────────────────────────────────────

int64_t rsg_i64_abs(int64_t n) {
    return n < 0 ? -n : n;
}

int64_t rsg_i64_min(int64_t n, int64_t other) {
    return n < other ? n : other;
}

int64_t rsg_i64_max(int64_t n, int64_t other) {
    return n > other ? n : other;
}

int64_t rsg_i64_clamp(int64_t n, int64_t lo, int64_t hi) {
    if (n < lo) {
        return lo;
    }
    if (n > hi) {
        return hi;
    }
    return n;
}

RsgStr rsg_i64_to_str(int64_t n) {
    return rsg_str_from_i64(n);
}

double rsg_i64_to_f64(int64_t n) {
    return (double)n;
}

int64_t rsg_i64_pow(int64_t n, uint32_t exp) {
    int64_t result = 1;
    int64_t base = n;
    while (exp > 0) {
        if (exp & 1) {
            result *= base;
        }
        base *= base;
        exp >>= 1;
    }
    return result;
}

// ── f64 extensions ────────────────────────────────────────────────────

double rsg_f64_abs(double n) {
    return fabs(n);
}

double rsg_f64_floor(double n) {
    return floor(n);
}

double rsg_f64_ceil(double n) {
    return ceil(n);
}

double rsg_f64_round(double n) {
    return round(n);
}

double rsg_f64_trunc(double n) {
    return trunc(n);
}

double rsg_f64_sqrt(double n) {
    return sqrt(n);
}

double rsg_f64_min(double n, double other) {
    return fmin(n, other);
}

double rsg_f64_max(double n, double other) {
    return fmax(n, other);
}

double rsg_f64_clamp(double n, double lo, double hi) {
    return fmin(fmax(n, lo), hi);
}

bool rsg_f64_is_nan(double n) {
    return isnan(n);
}

bool rsg_f64_is_inf(double n) {
    return isinf(n);
}

bool rsg_f64_is_finite(double n) {
    return isfinite(n);
}

int32_t rsg_f64_to_i32(double n) {
    return (int32_t)n;
}

RsgStr rsg_f64_to_str(double n) {
    return rsg_str_from_f64(n);
}

double rsg_f64_pow(double n, double exp) {
    return pow(n, exp);
}

double rsg_f64_ln(double n) {
    return log(n);
}

double rsg_f64_log2(double n) {
    return log2(n);
}

double rsg_f64_log10(double n) {
    return log10(n);
}

double rsg_f64_sin(double n) {
    return sin(n);
}

double rsg_f64_cos(double n) {
    return cos(n);
}

double rsg_f64_tan(double n) {
    return tan(n);
}

// ── f32 extensions ────────────────────────────────────────────────────

float rsg_f32_abs(float n) {
    return fabsf(n);
}

float rsg_f32_floor(float n) {
    return floorf(n);
}

float rsg_f32_ceil(float n) {
    return ceilf(n);
}

float rsg_f32_round(float n) {
    return roundf(n);
}

float rsg_f32_sqrt(float n) {
    return sqrtf(n);
}

bool rsg_f32_is_nan(float n) {
    return isnan(n);
}

bool rsg_f32_is_inf(float n) {
    return isinf(n);
}

bool rsg_f32_is_finite(float n) {
    return isfinite(n);
}

RsgStr rsg_f32_to_str(float n) {
    return rsg_str_from_f32(n);
}

// ── bool extensions ───────────────────────────────────────────────────

RsgStr rsg_bool_to_str(bool b) {
    return rsg_str_from_bool(b);
}

int32_t rsg_bool_to_i32(bool b) {
    return b ? 1 : 0;
}
