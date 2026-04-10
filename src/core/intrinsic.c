#include "core/intrinsic.h"

#include <stddef.h>
#include <string.h>

// ── Intrinsic descriptor table ──────────────────────────────────────

static const IntrinsicDesc INTRINSIC_TABLE[] = {
    {"print", INTRINSIC_PRINT, 1, 1, false, false},
    {"println", INTRINSIC_PRINTLN, 1, 1, false, false},
    {"assert", INTRINSIC_ASSERT, 1, 2, true, false},
    {"panic", INTRINSIC_PANIC, 1, 1, false, false},
    {"recover", INTRINSIC_RECOVER, 0, 0, false, false},
    {"len", INTRINSIC_LEN, 1, 1, false, true},
    {"rsg_slice_concat", INTRINSIC_SLICE_CONCAT, 2, 2, false, false},
};

#define INTRINSIC_COUNT (sizeof(INTRINSIC_TABLE) / sizeof(INTRINSIC_TABLE[0]))

IntrinsicKind intrinsic_lookup(const char *name) {
    if (name == NULL) {
        return INTRINSIC_NONE;
    }
    for (size_t i = 0; i < INTRINSIC_COUNT; i++) {
        if (strcmp(name, INTRINSIC_TABLE[i].name) == 0) {
            return INTRINSIC_TABLE[i].kind;
        }
    }
    // Try matching the base name before a "__" monomorphization suffix.
    // e.g. "print__i32" → base "print" → INTRINSIC_PRINT
    const char *dunder = strstr(name, "__");
    if (dunder != NULL) {
        size_t base_len = (size_t)(dunder - name);
        for (size_t i = 0; i < INTRINSIC_COUNT; i++) {
            if (strlen(INTRINSIC_TABLE[i].name) == base_len &&
                strncmp(name, INTRINSIC_TABLE[i].name, base_len) == 0) {
                return INTRINSIC_TABLE[i].kind;
            }
        }
    }
    return INTRINSIC_NONE;
}

const char *intrinsic_name(IntrinsicKind kind) {
    for (size_t i = 0; i < INTRINSIC_COUNT; i++) {
        if (INTRINSIC_TABLE[i].kind == kind) {
            return INTRINSIC_TABLE[i].name;
        }
    }
    return "<unknown>";
}

const IntrinsicDesc *intrinsic_desc(IntrinsicKind kind) {
    if (kind == INTRINSIC_NONE) {
        return NULL;
    }
    for (size_t i = 0; i < INTRINSIC_COUNT; i++) {
        if (INTRINSIC_TABLE[i].kind == kind) {
            return &INTRINSIC_TABLE[i];
        }
    }
    return NULL;
}
