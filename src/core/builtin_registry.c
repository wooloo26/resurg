#include "core/builtin_registry.h"

#include <stdio.h>
#include <string.h>

/**
 * @file builtin_registry.c
 * @brief Built-in function/member registration and lookup.
 */

// ── Initialization ────────────────────────────────────────────────

/** Register a single built-in function. */
static void register_fn(BuiltinRegistry *reg, const char *name, const Type *return_type,
                        BuiltinFnKind kind) {
    BuiltinFn *fn = rsg_malloc(sizeof(*fn));
    fn->name = name;
    fn->return_type = return_type;
    fn->kind = kind;
    hash_table_insert(&reg->fn_table, name, fn);
}

void builtin_registry_init(BuiltinRegistry *reg) {
    hash_table_init(&reg->fn_table, NULL);
    hash_table_init(&reg->member_table, NULL);

    // Built-in functions
    register_fn(reg, "print", &TYPE_UNIT_INST, BUILTIN_PRINT);
    register_fn(reg, "println", &TYPE_UNIT_INST, BUILTIN_PRINTLN);
    register_fn(reg, "assert", &TYPE_UNIT_INST, BUILTIN_ASSERT);
    register_fn(reg, "len", &TYPE_I32_INST, BUILTIN_LEN);
}

void builtin_registry_destroy(BuiltinRegistry *reg) {
    hash_table_destroy(&reg->fn_table);
    hash_table_destroy(&reg->member_table);
}

// ── Lookup ────────────────────────────────────────────────────────

const BuiltinFn *builtin_lookup_fn(const BuiltinRegistry *reg, const char *name) {
    return (const BuiltinFn *)hash_table_lookup(&reg->fn_table, name);
}

const BuiltinMember *builtin_lookup_member(const BuiltinRegistry *reg, const char *type_name,
                                           const char *member) {
    char key[128];
    snprintf(key, sizeof(key), "%s.%s", type_name, member);
    return (const BuiltinMember *)hash_table_lookup(&reg->member_table, key);
}
