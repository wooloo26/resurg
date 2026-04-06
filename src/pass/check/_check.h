#ifndef RSG__CHECK_H
#define RSG__CHECK_H

#include "pass/resolve/resolve.h"
#include "rsg/pass/check/check.h"

/**
 * @file _check.h
 * @brief Internal decls shared across check translation units.
 *
 * Not part of the pub API -- only included by pass/check/ and
 * pass/resolve/ files.
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

// ── Node dispatch (check_stmt.c) ──────────────────────────────────

/** Recursive AST walk - type-checks each node and returns its resolved type. */
const Type *check_node(Sema *sema, ASTNode *node);

// ── Expression checking (check_expr.c) ────────────────────────────

const Type *check_lit(Sema *sema, ASTNode *node);
const Type *check_id(Sema *sema, ASTNode *node);
const Type *check_unary(Sema *sema, ASTNode *node);
const Type *check_binary(Sema *sema, ASTNode *node);
const Type *check_call(Sema *sema, ASTNode *node);
const Type *check_member(Sema *sema, ASTNode *node);
const Type *check_idx(Sema *sema, ASTNode *node);
const Type *check_type_conversion(Sema *sema, ASTNode *node);
const Type *check_str_interpolation(Sema *sema, ASTNode *node);
const Type *check_array_lit(Sema *sema, ASTNode *node);
const Type *check_slice_lit(Sema *sema, ASTNode *node);
const Type *check_slice_expr(Sema *sema, ASTNode *node);
const Type *check_tuple_lit(Sema *sema, ASTNode *node);
const Type *check_struct_lit(Sema *sema, ASTNode *node);
const Type *check_address_of(Sema *sema, ASTNode *node);
const Type *check_deref(Sema *sema, ASTNode *node);
const Type *check_match(Sema *sema, ASTNode *node);
const Type *check_enum_init(Sema *sema, ASTNode *node);

// ── Statement checking (check_stmt.c) ─────────────────────────────

const Type *check_if(Sema *sema, ASTNode *node);
const Type *check_block(Sema *sema, ASTNode *node);
const Type *check_var_decl(Sema *sema, ASTNode *node);
void check_fn_body(Sema *sema, ASTNode *fn_node);
const Type *check_assign(Sema *sema, ASTNode *node);
const Type *check_compound_assign(Sema *sema, ASTNode *node);

#endif // RSG__CHECK_H
