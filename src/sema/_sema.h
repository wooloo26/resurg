#ifndef RG__SEMA_H
#define RG__SEMA_H

#include "rsg/sema.h"
#include "types/types.h"

/**
 * @file _sema.h
 * @brief Internal decls shared across sema translation units.
 *
 * Not part of the pub API -- only included by lib/middle/sema/ files.
 */

// ── Struct defs ─────────────────────────────────────────────────

/** Discriminator for Sym entries in the scope table. */
typedef enum {
    SYM_VAR,
    SYM_PARAM,
    SYM_FN,
    SYM_TYPE,
    SYM_MODULE,
} SymKind;

/** Sym table entry - one per declared name in a scope. */
struct Sym {
    const char *name;
    const Type *type;
    SymKind kind;
    bool is_pub;
    bool is_immut;
    const ASTNode *decl;
    Sym *owner;
};

/**
 * Lexical scope - hash table of Syms with a ptr to the
 * enclosing scope.
 */
struct Scope {
    HashTable table;         // name → Sym* (arena-backed)
    struct Scope *parent;    // enclosing scope (NULL for global)
    bool is_loop;            // true inside loop/for bodies (enables break/continue)
    const char *module_name; // propagated from the module decl
};

/** Type alias entry - registered during the first pass. */
typedef struct TypeAlias {
    const char *name;
    const Type *underlying;
} TypeAlias;

/**
 * Stored fn signature - registered in pass 1 so that forward calls
 * can be type-checked in pass 2.
 */
typedef struct FnSignature {
    const char *name;
    const Type *return_type;
    const Type **param_types; /* buf */
    const char **param_names; /* buf */
    int32_t param_count;
    bool is_pub;
} FnSignature;

/** A field def with its default value expr. */
typedef struct {
    const char *name;
    const Type *type;
    ASTNode *default_value; // may be NULL (field is required)
} StructFieldInfo;

/** A method def inside a struct. */
typedef struct {
    const char *name;
    bool is_mut_recv;
    bool is_ptr_recv;
    const char *recv_name;
    ASTNode *decl;
} StructMethodInfo;

/** Struct def — registered during the first pass. */
typedef struct {
    const char *name;
    StructFieldInfo *fields;   /* buf */
    StructMethodInfo *methods; /* buf */
    const char **embedded;     /* buf */
    const Type *type;          // resolved TYPE_STRUCT
} StructDef;

/** Enum def — registered during the first pass. */
typedef struct {
    const char *name;
    StructMethodInfo *methods; /* buf */
    const Type *type;          // resolved TYPE_ENUM
} EnumDef;

/** Pact (interface) def — registered during the first pass. */
typedef struct {
    const char *name;
    StructFieldInfo *fields;   /* buf - required fields */
    StructMethodInfo *methods; /* buf - all methods (required + default) */
    const char **super_pacts;  /* buf - constraint alias refs */
} PactDef;

struct Sema {
    Arena *arena;
    Scope *current_scope;
    int32_t err_count;
    const Type *loop_break_type; // break value type in current loop (NULL if no break-with-value)
    HashTable type_alias_table;  // name → const Type*
    HashTable fn_table;          // name → FnSignature*
    HashTable struct_table;      // name → StructDef*
    HashTable enum_table;        // name → EnumDef*
    HashTable pact_table;        // name → PactDef*
};

/** Report a semantic err and bump the analyzer's err counter. */
#define SEMA_ERR(analyzer, loc, ...)                                                               \
    do {                                                                                           \
        rsg_err(loc, __VA_ARGS__);                                                                 \
        (analyzer)->err_count++;                                                                   \
    } while (0)

// ── Scope manipulation (scope.c) ───────────────────────────────────────

/** Grouped params for defining a sym in a scope. */
typedef struct {
    const char *name;
    const Type *type;
    bool is_pub;
    SymKind kind;
} SymDef;

/** Push a new child scope.  If @p is_loop is true, break/continue are legal inside it. */
Scope *scope_push(Sema *analyzer, bool is_loop);
/** Pop the innermost scope. */
void scope_pop(Sema *analyzer);
/** Define a sym in the current scope. */
void scope_define(Sema *analyzer, const SymDef *def);
/** Look up @p name in the innermost scope only (for redef checks). */
Sym *scope_lookup_current(const Sema *analyzer, const char *name);
/** Walk the scope chain outward to find @p name. */
Sym *scope_lookup(const Sema *analyzer, const char *name);
/** Return true if any enclosing scope has is_loop set. */
bool in_loop(const Sema *analyzer);

// ── Type resolution (resolve.c) ────────────────────────────────────────

/** Look up a type alias by name.  Returns the underlying type or NULL. */
const Type *sema_lookup_type_alias(const Sema *analyzer, const char *name);
/** Look up a fn signature by name. */
FnSignature *sema_lookup_fn(const Sema *analyzer, const char *name);
/** Look up a struct def by name. */
StructDef *sema_lookup_struct(const Sema *analyzer, const char *name);
/** Look up an enum def by name. */
EnumDef *sema_lookup_enum(const Sema *analyzer, const char *name);
/** Look up a pact def by name. */
PactDef *sema_lookup_pact(const Sema *analyzer, const char *name);
/**
 * Map a syntactic ASTType to a resolved Type*.  Returns NULL for inferred
 * types; emits an err and returns TYPE_ERR for unknown names.
 */
const Type *resolve_ast_type(Sema *analyzer, const ASTType *ast_type);
/** Map a lit kind to its corresponding type. */
const Type *lit_kind_to_type(LitKind kind);
/** Return the LitKind for a given TypeKind. */
LitKind type_to_lit_kind(TypeKind kind);
/**
 * Promote a lit node to match @p target's numeric type.
 * Returns @p target on success, NULL if no promotion applies.
 */
const Type *promote_lit(ASTNode *lit, const Type *target);

// ── Node dispatch (stmt.c) ────────────────────────────────────────

/** Recursive AST walk - type-checks each node and returns its resolved type. */
const Type *check_node(Sema *analyzer, ASTNode *node);

// ── Expression checking (expr.c) ─────────────────────────────────

const Type *check_lit(Sema *analyzer, ASTNode *node);
const Type *check_id(Sema *analyzer, ASTNode *node);
const Type *check_unary(Sema *analyzer, ASTNode *node);
const Type *check_binary(Sema *analyzer, ASTNode *node);
const Type *check_call(Sema *analyzer, ASTNode *node);
const Type *check_member(Sema *analyzer, ASTNode *node);
const Type *check_idx(Sema *analyzer, ASTNode *node);
const Type *check_type_conversion(Sema *analyzer, ASTNode *node);
const Type *check_str_interpolation(Sema *analyzer, ASTNode *node);
const Type *check_array_lit(Sema *analyzer, ASTNode *node);
const Type *check_slice_lit(Sema *analyzer, ASTNode *node);
const Type *check_slice_expr(Sema *analyzer, ASTNode *node);
const Type *check_tuple_lit(Sema *analyzer, ASTNode *node);
const Type *check_struct_lit(Sema *analyzer, ASTNode *node);
const Type *check_address_of(Sema *analyzer, ASTNode *node);
const Type *check_deref(Sema *analyzer, ASTNode *node);
const Type *check_match(Sema *analyzer, ASTNode *node);
const Type *check_enum_init(Sema *analyzer, ASTNode *node);

// ── Statement checking (stmt.c) ───────────────────────────────────

const Type *check_if(Sema *analyzer, ASTNode *node);
const Type *check_block(Sema *analyzer, ASTNode *node);
const Type *check_var_decl(Sema *analyzer, ASTNode *node);
void check_fn_body(Sema *analyzer, ASTNode *fn_node);
const Type *check_assign(Sema *analyzer, ASTNode *node);
const Type *check_compound_assign(Sema *analyzer, ASTNode *node);

#endif // RG__SEMA_H
