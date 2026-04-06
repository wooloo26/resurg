#ifndef RG__LOWERING_H
#define RG__LOWERING_H

#include "ast/ast.h"
#include "lowering.h"
#include "sema/_sema.h"

/**
 * @file _lowering.h
 * @brief Internal decls shared across lowering translation units.
 *
 * Not part of the pub API -- only included by src/lowering/ files.
 */

// ── Struct defs ─────────────────────────────────────────────────

typedef struct LoweringScope LoweringScope;

struct LoweringScope {
    HashTable table;
    LoweringScope *parent;
};

struct Lowering {
    Arena *tt_arena;
    LoweringScope *scope;
    int32_t err_count;
    const char *current_module;
    int32_t temp_counter;
    int32_t shadow_counter;
    const Type **compound_types; /* buf */
    TTSym *current_recv;         // non-NULL inside method body
    const char *current_recv_name;
    bool current_is_ptr_recv; // true if current method has ptr recv
};

// ── Scope manipulation ────────────────────────────────────────────────

void lowering_scope_enter(Lowering *low);
void lowering_scope_leave(Lowering *low);
void lowering_scope_add(Lowering *low, const char *name, TTSym *sym);
TTSym *lowering_scope_find(const Lowering *low, const char *name);

// ── Parameter structs ──────────────────────────────────────────────────

/** Grouped params for creating a TTSym. */
typedef struct {
    TtSymKind kind;
    const char *name;
    const Type *type;
    bool is_mut;
    SourceLoc loc;
} TtSymSpec;

/** Grouped params for resolving a promoted field. */
typedef struct {
    TTNode *object;
    const Type *struct_type;
    const char *field_name;
    bool via_ptr;
    SourceLoc loc;
} FieldLookup;

/** Grouped params for creating an integer lit node. */
typedef struct {
    uint64_t value;
    const Type *type;
    TypeKind int_kind;
    SourceLoc loc;
} IntLitSpec;

/** Grouped params for creating a builtin call node. */
typedef struct {
    const char *name;
    const Type *return_type;
    TTNode **args;
    SourceLoc loc;
} BuiltinCallSpec;

// ── Shared helpers ────────────────────────────────────────────────────

/** Create a Sema Sym and wrap it in a TTSym. */
TTSym *lowering_make_sym(Lowering *low, const TtSymSpec *spec);
/** Create a var sym, register it in scope, and return it. */
TTSym *lowering_add_var(Lowering *low, const TtSymSpec *spec);
/** Generate a unique temp name like _tt_tmp_0. */
const char *lowering_make_temp_name(Lowering *low);
/** Create a TtVarRef node. */
TTNode *lowering_make_var_ref(Lowering *low, TTSym *sym, SourceLoc loc);
/** Create a TtIntLit node. */
TTNode *lowering_make_int_lit(Lowering *low, const IntLitSpec *spec);
/** Create a TT_VAR_DECL node — derives name/type/mut from @p sym. */
TTNode *lowering_make_var_decl(Lowering *low, TTSym *sym, TTNode *init);
/** Map a compound-assignment TokenKind to its base arithmetic operator. */
TokenKind lowering_compound_to_base_op(TokenKind op);
/**
 * Look up a field in the embedded structs of a struct type.
 * Returns a two-level TT_STRUCT_FIELD_ACCESS chain (embed → field) on hit, or NULL.
 */
TTNode *lowering_resolve_promoted_field(Lowering *low, const FieldLookup *lookup);
/** Create a TtCall node for a builtin runtime fn. */
TTNode *lowering_make_builtin_call(Lowering *low, const BuiltinCallSpec *spec);

// ── Cross-file dispatch ───────────────────────────────────────────────

/** Lower any AST node (dispatches to decl/stmt/expr handlers). */
TTNode *lower_node(Lowering *low, const ASTNode *ast);
/** Lower an AST expr. */
TTNode *lower_expr(Lowering *low, const ASTNode *ast);
/** Lower a NODE_BLOCK. */
TTNode *lower_block(Lowering *low, const ASTNode *ast);
/** Lower a NODE_IF (used as both expr and stmt). */
TTNode *lower_stmt_if(Lowering *low, const ASTNode *ast);
/** Lower a NODE_STR_INTERPOLATION. */
TTNode *lower_str_interpolation(Lowering *low, const ASTNode *ast);
/** Lower a NODE_MATCH. */
TTNode *lower_match(Lowering *low, const ASTNode *ast);
/** Lower a NODE_FN_DECL. */
TTNode *lower_fn_decl(Lowering *low, const ASTNode *ast);
/** Lower a method decl inside a struct. */
TTNode *lower_method_decl(Lowering *low, const ASTNode *ast, const char *struct_name,
                          const Type *struct_type);

#endif // RG__LOWERING_H
