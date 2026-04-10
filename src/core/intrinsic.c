#include "core/intrinsic.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "core/common.h"

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

// ── Hash table for O(1) lookup ──────────────────────────────────────

static HashTable g_intrinsic_map;
static bool g_intrinsic_map_ready;

static void intrinsic_ensure_map(void) {
    if (g_intrinsic_map_ready) {
        return;
    }
    hash_table_init(&g_intrinsic_map, NULL);
    for (size_t i = 0; i < INTRINSIC_COUNT; i++) {
        hash_table_insert(&g_intrinsic_map, INTRINSIC_TABLE[i].name,
                          (void *)(uintptr_t)INTRINSIC_TABLE[i].kind);
    }
    g_intrinsic_map_ready = true;
}

IntrinsicKind intrinsic_lookup(const char *name) {
    if (name == NULL) {
        return INTRINSIC_NONE;
    }
    intrinsic_ensure_map();

    // Exact match via hash table — O(1).
    void *val = hash_table_lookup(&g_intrinsic_map, name);
    if (val != NULL) {
        return (IntrinsicKind)(uintptr_t)val;
    }

    // Strip module prefix: "module.print" → "print"
    const char *dot = strrchr(name, '.');
    if (dot != NULL) {
        val = hash_table_lookup(&g_intrinsic_map, dot + 1);
        if (val != NULL) {
            return (IntrinsicKind)(uintptr_t)val;
        }
    }

    // Try matching the base name before a "__" monomorphization suffix.
    // e.g. "print__i32" → base "print" → INTRINSIC_PRINT.
    // Only match the *first* "__" to prevent false positives on
    // user-defined names that happen to contain "__" later.
    const char *dunder = strstr(name, "__");
    if (dunder != NULL && dunder != name) {
        size_t base_len = (size_t)(dunder - name);
        char buf[64];
        if (base_len < sizeof(buf)) {
            memcpy(buf, name, base_len);
            buf[base_len] = '\0';
            // Also strip module prefix from the base
            const char *base = buf;
            const char *base_dot = strrchr(buf, '.');
            if (base_dot != NULL) {
                base = base_dot + 1;
            }
            val = hash_table_lookup(&g_intrinsic_map, base);
            if (val != NULL) {
                return (IntrinsicKind)(uintptr_t)val;
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

// ── Runtime ABI default table ──────────────────────────────────────────

static const RuntimeABI C17_ABI = {
    .gc_init = "rsg_gc_init",
    .gc_collect = "rsg_gc_collect",
    .str_new = "rsg_str_new",
    .str_concat = RSG_FN_STR_CONCAT,
    .str_equal = RSG_FN_STR_EQUAL,
    .str_from = RSG_FN_STR_FROM,
    .slice_new = "rsg_slice_new",
    .slice_from_array = "rsg_slice_from_array",
    .slice_sub = "rsg_slice_sub",
    .slice_concat = RSG_FN_SLICE_CONCAT,
    .panic = RSG_FN_PANIC,
    .recover = RSG_FN_RECOVER,
    .assert = RSG_FN_ASSERT,
    .panic_push = "rsg_panic_push",
    .panic_pop = "rsg_panic_pop",
    .print = "rsgu_print_",
    .println = "rsgu_println_",
    .runtime_header = "rsg_runtime.h",
};

const RuntimeABI *runtime_abi_default(void) {
    return &C17_ABI;
}
