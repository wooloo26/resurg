#ifndef RSG_PRIM_H
#define RSG_PRIM_H

/**
 * @file rsg_prim.h
 * @brief Extension methods for numeric and bool primitive types.
 */

#include <stdbool.h>
#include <stdint.h>

#include "rsg_str.h"

// ── i32 extensions ────────────────────────────────────────────────────

int32_t rsg_i32_abs(int32_t n);
int32_t rsg_i32_min(int32_t n, int32_t other);
int32_t rsg_i32_max(int32_t n, int32_t other);
int32_t rsg_i32_clamp(int32_t n, int32_t lo, int32_t hi);
RsgStr rsg_i32_to_str(int32_t n);
double rsg_i32_to_f64(int32_t n);
int32_t rsg_i32_pow(int32_t n, uint32_t exp);

// ── i64 extensions ────────────────────────────────────────────────────

int64_t rsg_i64_abs(int64_t n);
int64_t rsg_i64_min(int64_t n, int64_t other);
int64_t rsg_i64_max(int64_t n, int64_t other);
int64_t rsg_i64_clamp(int64_t n, int64_t lo, int64_t hi);
RsgStr rsg_i64_to_str(int64_t n);
double rsg_i64_to_f64(int64_t n);
int64_t rsg_i64_pow(int64_t n, uint32_t exp);

// ── f64 extensions ────────────────────────────────────────────────────

double rsg_f64_abs(double n);
double rsg_f64_floor(double n);
double rsg_f64_ceil(double n);
double rsg_f64_round(double n);
double rsg_f64_trunc(double n);
double rsg_f64_sqrt(double n);
double rsg_f64_min(double n, double other);
double rsg_f64_max(double n, double other);
double rsg_f64_clamp(double n, double lo, double hi);
bool rsg_f64_is_nan(double n);
bool rsg_f64_is_inf(double n);
bool rsg_f64_is_finite(double n);
int32_t rsg_f64_to_i32(double n);
RsgStr rsg_f64_to_str(double n);
double rsg_f64_pow(double n, double exp);
double rsg_f64_ln(double n);
double rsg_f64_log2(double n);
double rsg_f64_log10(double n);
double rsg_f64_sin(double n);
double rsg_f64_cos(double n);
double rsg_f64_tan(double n);

// ── f32 extensions ────────────────────────────────────────────────────

float rsg_f32_abs(float n);
float rsg_f32_floor(float n);
float rsg_f32_ceil(float n);
float rsg_f32_round(float n);
float rsg_f32_sqrt(float n);
bool rsg_f32_is_nan(float n);
bool rsg_f32_is_inf(float n);
bool rsg_f32_is_finite(float n);
RsgStr rsg_f32_to_str(float n);

// ── bool extensions ───────────────────────────────────────────────────

RsgStr rsg_bool_to_str(bool b);
int32_t rsg_bool_to_i32(bool b);

#endif // RSG_PRIM_H
