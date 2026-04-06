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
    LoweringScope *scope;
    int32_t error_count;
    const char *current_module;
    int32_t temp_counter;
    int32_t shadow_counter;
    const Type **compound_types; /* buf */
    TtSymbol *current_receiver;  // non-NULL inside method body
    const char *current_receiver_name;
};

// ── Scope manipulation ────────────────────────────────────────────────

void lowering_scope_enter(Lowering *low);
void lowering_scope_leave(Lowering *low);
void lowering_scope_add(Lowering *low, const char *name, TtSymbol *symbol);
TtSymbol *lowering_scope_find(const Lowering *low, const char *name);

// ── Parameter structs ──────────────────────────────────────────────────

/** Grouped parameters for creating a TtSymbol. */
typedef struct {
    TtSymbolKind kind;
    const char *name;
    const Type *type;
    bool is_mut;
    SourceLocation location;
} TtSymbolSpec;

/** Grouped parameters for resolving a promoted field. */
typedef struct {
    TtNode *object;
    const Type *struct_type;
    const char *field_name;
    bool via_pointer;
    SourceLocation location;
} FieldLookup;

/** Grouped parameters for creating an integer literal node. */
typedef struct {
    uint64_t value;
    const Type *type;
    TypeKind int_kind;
    SourceLocation location;
} IntLitSpec;

/** Grouped parameters for creating a builtin call node. */
typedef struct {
    const char *name;
    const Type *return_type;
    TtNode **args;
    SourceLocation location;
} BuiltinCallSpec;

// ── Shared helpers ────────────────────────────────────────────────────

/** Create a Sema Symbol and wrap it in a TtSymbol. */
TtSymbol *lowering_make_symbol(Lowering *low, const TtSymbolSpec *spec);
/** Create a variable symbol, register it in scope, and return it. */
TtSymbol *lowering_add_variable(Lowering *low, const TtSymbolSpec *spec);
/** Generate a unique temporary name like _tt_tmp_0. */
const char *lowering_make_temp_name(Lowering *low);
/** Create a TtVarRef node. */
TtNode *lowering_make_var_ref(Lowering *low, TtSymbol *symbol, SourceLocation location);
/** Create a TtIntLit node. */
TtNode *lowering_make_int_lit(Lowering *low, const IntLitSpec *spec);
/** Create a TT_VARIABLE_DECLARATION node — derives name/type/mut from @p symbol. */
TtNode *lowering_make_var_decl(Lowering *low, TtSymbol *symbol, TtNode *initializer);
/** Map a compound-assignment TokenKind to its base arithmetic operator. */
TokenKind lowering_compound_to_base_op(TokenKind op);
/**
 * Look up a field in the embedded structs of a struct type.
 * Returns a two-level TT_STRUCT_FIELD_ACCESS chain (embed → field) on hit, or NULL.
 */
TtNode *lowering_resolve_promoted_field(Lowering *low, const FieldLookup *lookup);
/** Create a TtCall node for a builtin runtime function. */
TtNode *lowering_make_builtin_call(Lowering *low, const BuiltinCallSpec *spec);

// ── Cross-file dispatch ───────────────────────────────────────────────

/** Lower any AST node (dispatches to decl/stmt/expr handlers). */
TtNode *lower_node(Lowering *low, const ASTNode *ast);
/** Lower an AST expression. */
TtNode *lower_expression(Lowering *low, const ASTNode *ast);
/** Lower a NODE_BLOCK. */
TtNode *lower_block(Lowering *low, const ASTNode *ast);
/** Lower a NODE_IF (used as both expression and statement). */
TtNode *lower_statement_if(Lowering *low, const ASTNode *ast);
/** Lower a NODE_STRING_INTERPOLATION. */
TtNode *lower_string_interpolation(Lowering *low, const ASTNode *ast);
/** Lower a NODE_MATCH. */
TtNode *lower_match(Lowering *low, const ASTNode *ast);
/** Lower a NODE_FUNCTION_DECLARATION. */
TtNode *lower_function_declaration(Lowering *low, const ASTNode *ast);
/** Lower a method declaration inside a struct. */
TtNode *lower_method_declaration(Lowering *low, const ASTNode *ast, const char *struct_name,
                                 const Type *struct_type);

#endif // RG__LOWERING_H
