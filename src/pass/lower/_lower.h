#ifndef RSG__LOWER_H
#define RSG__LOWER_H

#include "lower.h"
#include "repr/ast.h"

/**
 * @file _lower.h
 * @brief Internal decls shared across lower translation units.
 *
 * Not part of the pub API -- only included by src/pass/lower/ files.
 */

// ── Struct defs ─────────────────────────────────────────────────

typedef struct LoweringScope LoweringScope;

struct LoweringScope {
    HashTable table;
    LoweringScope *parent;
};

struct Lower {
    Arena *hir_arena;
    LoweringScope *scope;
    int32_t err_count;
    const char *current_module;
    int32_t temp_counter;
    int32_t shadow_counter;
    const Type **compound_types; /* buf */
    HirSym *current_recv;        // non-NULL inside method body
    const char *current_recv_name;
    bool current_is_ptr_recv; // true if current method has ptr recv
};

// ── Scope manipulation ────────────────────────────────────────────────

void lower_scope_enter(Lower *low);
void lower_scope_leave(Lower *low);
void lower_scope_define(Lower *low, const char *name, HirSym *sym);
HirSym *lower_scope_lookup(const Lower *low, const char *name);

// ── Parameter structs ──────────────────────────────────────────────────

/** Grouped params for resolving a promoted field. */
typedef struct {
    HirNode *object;
    const Type *struct_type;
    const char *field_name;
    bool via_ptr;
    SrcLoc loc;
} FieldLookup;

/** Grouped params for creating an integer lit node. */
typedef struct {
    uint64_t value;
    const Type *type;
    TypeKind int_kind;
    SrcLoc loc;
} IntLitSpec;

/** Grouped params for creating a builtin call node. */
typedef struct {
    const char *name;
    const Type *return_type;
    HirNode **args;
    SrcLoc loc;
} BuiltinCallSpec;

// ── Shared helpers ────────────────────────────────────────────────────

/** Create a HirSym from the given spec. */
HirSym *lower_make_sym(Lower *low, const HirSymSpec *spec);
/** Create a var sym, register it in scope, and return it. */
HirSym *lower_add_var(Lower *low, const HirSymSpec *spec);
/** Generate a unique temp name like _hir_tmp_0. */
const char *lower_make_temp_name(Lower *low);
/** Create a HirVarRef node. */
HirNode *lower_make_var_ref(Lower *low, HirSym *sym, SrcLoc loc);
/** Create a HirIntLit node. */
HirNode *lower_make_int_lit(Lower *low, const IntLitSpec *spec);
/** Create a HIR_VAR_DECL node — derives name/type/mut from @p sym. */
HirNode *lower_make_var_decl(Lower *low, HirSym *sym, HirNode *init);
/** Map a compound-assignment TokenKind to its base arithmetic operator. */
TokenKind lower_compound_to_base_op(TokenKind op);
/**
 * Look up a field in the embedded structs of a struct type.
 * Returns a two-level HIR_STRUCT_FIELD_ACCESS chain (embed → field) on hit, or NULL.
 */
HirNode *lower_resolve_promoted_field(Lower *low, const FieldLookup *lookup);
/** Create a HirCall node for a builtin runtime fn. */
HirNode *lower_make_builtin_call(Lower *low, const BuiltinCallSpec *spec);

// ── Cross-file dispatch ───────────────────────────────────────────────

/** Lower any AST node (dispatches to decl/stmt/expr handlers). */
HirNode *lower_node(Lower *low, const ASTNode *ast);
/** Lower an AST expr. */
HirNode *lower_expr(Lower *low, const ASTNode *ast);
/** Lower a NODE_BLOCK. */
HirNode *lower_block(Lower *low, const ASTNode *ast);
/** Lower a NODE_IF (used as both expr and stmt). */
HirNode *lower_stmt_if(Lower *low, const ASTNode *ast);
/** Lower a NODE_LOOP. */
HirNode *lower_loop(Lower *low, const ASTNode *ast);
/** Lower a NODE_WHILE. */
HirNode *lower_while(Lower *low, const ASTNode *ast);
/** Lower a NODE_FOR (dispatches to slice or range variant). */
HirNode *lower_for(Lower *low, const ASTNode *ast);
/** Lower a NODE_STR_INTERPOLATION. */
HirNode *lower_str_interpolation(Lower *low, const ASTNode *ast);
/** Lower a NODE_MATCH. */
HirNode *lower_match(Lower *low, const ASTNode *ast);
/** Lower a NODE_FN_DECL. */
HirNode *lower_fn_decl(Lower *low, const ASTNode *ast);
/** Lower a method decl inside a struct. */
HirNode *lower_method_decl(Lower *low, const ASTNode *ast, const char *struct_name,
                           const Type *struct_type);

#endif // RSG__LOWER_H
