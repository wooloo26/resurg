#include "rsg_testing.h"

#include <stdio.h>

#include "rsg_panic.h"

// ── Helpers ───────────────────────────────────────────────────────────

static void fail_eq_i32(const char *label, int32_t actual, int32_t expected) {
    char buf[256];
    snprintf(buf, sizeof(buf), "assert_%s failed: expected %d, got %d", label, expected, actual);
    rsg_panic(buf);
}

static void fail_eq_str(const char *label, RsgStr actual, RsgStr expected) {
    char buf[512];
    snprintf(buf, sizeof(buf), "assert_%s failed: expected \"%.*s\", got \"%.*s\"", label,
             expected.len, expected.data, actual.len, actual.data);
    rsg_panic(buf);
}

// ── assert_eq ─────────────────────────────────────────────────────────

void rsg_testing_assert_eq_i32(int32_t actual, int32_t expected) {
    if (actual != expected) {
        fail_eq_i32("eq", actual, expected);
    }
}

void rsg_testing_assert_eq_str(RsgStr actual, RsgStr expected) {
    if (!rsg_str_equal(actual, expected)) {
        fail_eq_str("eq", actual, expected);
    }
}

void rsg_testing_assert_eq_f64(double actual, double expected) {
    if (actual != expected) {
        char buf[256];
        snprintf(buf, sizeof(buf), "assert_eq failed: expected %g, got %g", expected, actual);
        rsg_panic(buf);
    }
}

void rsg_testing_assert_eq_bool(bool actual, bool expected) {
    if (actual != expected) {
        char buf[128];
        snprintf(buf, sizeof(buf), "assert_eq failed: expected %s, got %s",
                 expected ? "true" : "false", actual ? "true" : "false");
        rsg_panic(buf);
    }
}

// ── assert_ne ─────────────────────────────────────────────────────────

void rsg_testing_assert_ne_i32(int32_t actual, int32_t expected) {
    if (actual == expected) {
        fail_eq_i32("ne", actual, expected);
    }
}

void rsg_testing_assert_ne_str(RsgStr actual, RsgStr expected) {
    if (rsg_str_equal(actual, expected)) {
        fail_eq_str("ne", actual, expected);
    }
}
