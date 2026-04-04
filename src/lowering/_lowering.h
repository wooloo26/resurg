#ifndef RG__LOWERING_H
#define RG__LOWERING_H

#include "ast/ast.h"
#include "lowering.h"
#include "sema/_sema.h"

/**
 * @file _lowering.h
 * @brief Internal declarations shared across lowering translation units.
 *
 * Not part of the public API -- only included by src/lowering/ files.
 */

// ── Struct definitions ─────────────────────────────────────────────────

typedef struct LoweringScope LoweringScope;

struct LoweringScope {
    HashTable table;
    LoweringScope *parent;
};

struct Lowering {
    Arena *tt_arena;
    Arena *sema_arena;
    LoweringScope *scope;
    int32_t error_count;
    const char *current_module;
    int32_t temp_counter;
    int32_t shadow_counter;
    const Type **compound_types; /* buf */
};

// ── Scope manipulation ────────────────────────────────────────────────

void lowering_scope_enter(Lowering *low);
void lowering_scope_leave(Lowering *low);
void lowering_scope_add(Lowering *low, const char *name, TtSymbol *symbol);
TtSymbol *lowering_scope_find(const Lowering *low, const char *name);

// ── Shared helpers ────────────────────────────────────────────────────

/** Create a Sema Symbol and wrap it in a TtSymbol. */
TtSymbol *lowering_make_symbol(Lowering *low, TtSymbolKind kind, const char *name, const Type *type,
                               bool is_mut, SourceLocation location);
/** Generate a unique temporary name like _tt_tmp_0. */
const char *lowering_make_temp_name(Lowering *low);
/** Create a TtVarRef node. */
TtNode *lowering_make_var_ref(Lowering *low, TtSymbol *symbol, SourceLocation location);
/** Create a TtIntLit node. */
TtNode *lowering_make_int_lit(Lowering *low, uint64_t value, const Type *type, TypeKind int_kind,
                              SourceLocation location);
/** Map a compound-assignment TokenKind to its base arithmetic operator. */
TokenKind lowering_compound_to_base_op(TokenKind op);
/** Create a TtCall node for a builtin runtime function. */
TtNode *lowering_make_builtin_call(Lowering *low, const char *name, const Type *return_type,
                                   TtNode **args, SourceLocation location);

// ── Cross-file dispatch ───────────────────────────────────────────────

/** Lower any AST node (dispatches to decl/stmt/expr handlers). */
TtNode *lower_node(Lowering *low, const ASTNode *ast);
/** Lower an AST expression. */
TtNode *lower_expression(Lowering *low, const ASTNode *ast);
/** Lower a NODE_BLOCK. */
TtNode *lower_block(Lowering *low, const ASTNode *ast);
/** Lower a NODE_IF (used as both expression and statement). */
TtNode *lower_statement_if(Lowering *low, const ASTNode *ast);
/** Lower a NODE_FUNCTION_DECLARATION. */
TtNode *lower_function_declaration(Lowering *low, const ASTNode *ast);

#endif // RG__LOWERING_H
