#ifndef RSG__SEMA_H
#define RSG__SEMA_H

#include "pass/resolve/resolve.h"

/**
 * @file _sema.h
 * @brief Shared semantic context — Sema struct and registration types.
 *
 * Included by both check and resolve translation units.
 * Separates the semantic context from the check dispatch functions.
 */

// ── Registration structs ────────────────────────────────────────

/** Type alias entry - registered during the first pass. */
typedef struct TypeAlias {
    const char *name;
    const Type *underlying;
} TypeAlias;

/**
 * Stored fn sig - registered in pass 1 so that forward calls
 * can be type-checked in pass 2.
 */
struct FnSig {
    const char *name;
    const Type *return_type;
    const Type **param_types; /* buf */
    const char **param_names; /* buf */
    int32_t param_count;
    bool is_pub;
};

/** A field def with its default value expr. */
typedef struct StructFieldInfo {
    const char *name;
    const Type *type;
    ASTNode *default_value; // may be NULL (field is required)
} StructFieldInfo;

/** A method def inside a struct. */
typedef struct StructMethodInfo {
    const char *name;
    bool is_mut_recv;
    bool is_ptr_recv;
    const char *recv_name;
    ASTNode *decl;
} StructMethodInfo;

/** Struct def — registered during the first pass. */
struct StructDef {
    const char *name;
    StructFieldInfo *fields;   /* buf */
    StructMethodInfo *methods; /* buf */
    const char **embedded;     /* buf */
    const Type *type;          // resolved TYPE_STRUCT
};

/** Enum def — registered during the first pass. */
struct EnumDef {
    const char *name;
    StructMethodInfo *methods; /* buf */
    const Type *type;          // resolved TYPE_ENUM
};

/** Pact (interface) def — registered during the first pass. */
struct PactDef {
    const char *name;
    StructFieldInfo *fields;   /* buf - required fields */
    StructMethodInfo *methods; /* buf - all methods (required + default) */
    const char **super_pacts;  /* buf - constraint alias refs */
};

struct Sema {
    Arena *arena;
    Scope *current_scope;
    int32_t err_count;
    const Type *loop_break_type; // break value type in current loop (NULL if no break-with-value)
    HashTable type_alias_table;  // name → const Type*
    HashTable fn_table;          // name → FnSig*
    HashTable struct_table;      // name → StructDef*
    HashTable enum_table;        // name → EnumDef*
    HashTable pact_table;        // name → PactDef*
};

/** Report a semantic err and bump the sema's err counter. */
#define SEMA_ERR(sema, loc, ...)                                                                   \
    do {                                                                                           \
        rsg_err(loc, __VA_ARGS__);                                                                 \
        (sema)->err_count++;                                                                       \
    } while (0)

#endif // RSG__SEMA_H
