#ifndef RSG__BUILTIN_REGISTRY_H
#define RSG__BUILTIN_REGISTRY_H

#include "core/common.h"
#include "repr/types.h"

/**
 * @file builtin_registry.h
 * @brief Centralized registry for built-in functions and member accessors.
 *
 * Replaces scattered strcmp checks across check/lower with table lookups.
 */

// ── Built-in function kinds ───────────────────────────────────────

typedef enum {
    BUILTIN_PRINT,
    BUILTIN_PRINTLN,
    BUILTIN_ASSERT,
    BUILTIN_LEN,
} BuiltinFnKind;

/** Descriptor for a compiler built-in function. */
typedef struct {
    const char *name;
    const Type *return_type;
    BuiltinFnKind kind;
} BuiltinFn;

// ── Built-in member kinds ─────────────────────────────────────────

typedef enum {
    BUILTIN_MEMBER_FIELD,
} BuiltinMemberKind;

/** Descriptor for a compiler built-in member accessor (e.g. slice.len). */
typedef struct {
    const char *type_name;
    const char *member_name;
    const Type *result_type;
    BuiltinMemberKind kind;
} BuiltinMember;

// ── Registry ──────────────────────────────────────────────────────

typedef struct {
    HashTable fn_table;     // name → BuiltinFn*
    HashTable member_table; // "type.member" → BuiltinMember*
} BuiltinRegistry;

/** Populate the registry with all built-in functions and members. */
void builtin_registry_init(BuiltinRegistry *reg);

/** Free resources owned by the registry. */
void builtin_registry_destroy(BuiltinRegistry *reg);

/** Look up a built-in function by name. Returns NULL if not built-in. */
const BuiltinFn *builtin_lookup_fn(const BuiltinRegistry *reg, const char *name);

/** Look up a built-in member by type kind and member name. Returns NULL if not built-in. */
const BuiltinMember *builtin_lookup_member(const BuiltinRegistry *reg, const char *type_name,
                                           const char *member);

#endif // RSG__BUILTIN_REGISTRY_H
