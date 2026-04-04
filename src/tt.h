#ifndef RG_TT_H
#define RG_TT_H

#include "common.h"
#include "token.h"
#include "types.h"

/**
 * @file tt.h
 * @brief Typed Tree — fully typed, desugared intermediate representation.
 *
 * Every TT node carries a resolved type (never NULL), desugared constructs,
 * scope-resolved identifiers (TtSymbol wrapping Sema's Symbol), and no
 * syntactic sugar.
 */

typedef struct Symbol Symbol;
typedef struct TtNode TtNode;
typedef struct TtSymbol TtSymbol;

// ── TtSymbol ───────────────────────────────────────────────────────────

/** Symbol kind in the TT — authoritative for lowering and codegen. */
typedef enum {
    TT_SYM_VAR,
    TT_SYM_PARAM,
    TT_SYM_FUNCTION,
    TT_SYM_TYPE,
    TT_SYM_MODULE,
} TtSymbolKind;

/**
 * TT symbol — wraps Sema's Symbol with additional lowering metadata.
 * TtSymbol.kind is authoritative in TT/codegen; accessors delegate to
 * Sema for name and type.
 */
struct TtSymbol {
    TtSymbolKind kind;
    Symbol *sema_symbol;
    bool is_mut;
    const char *mangled_name;
    SourceLocation location;
};

/** Return the name of a TtSymbol (delegates to sema_symbol). */
const char *tt_symbol_name(const TtSymbol *symbol);
/** Return the type of a TtSymbol (delegates to sema_symbol). */
const Type *tt_symbol_type(const TtSymbol *symbol);

// ── TtNodeKind ─────────────────────────────────────────────────────────

/** Discriminator for TtNode tagged union. */
typedef enum {
    // Top-level
    TT_FILE,
    TT_MODULE,
    TT_TYPE_ALIAS,

    // Declarations
    TT_FUNCTION_DECL,
    TT_PARAM,

    // Statements
    TT_VAR_DECL,
    TT_RETURN,
    TT_ASSIGN,
    TT_BREAK,
    TT_CONTINUE,
    TT_EXPR_STMT,

    // Literals
    TT_BOOL_LIT,
    TT_INT_LIT,
    TT_FLOAT_LIT,
    TT_CHAR_LIT,
    TT_STRING_LIT,
    TT_UNIT_LIT,
    TT_ARRAY_LIT,
    TT_TUPLE_LIT,

    // References
    TT_VAR_REF,
    TT_MODULE_ACCESS,
    TT_INDEX,
    TT_TUPLE_INDEX,

    // Operations
    TT_UNARY,
    TT_BINARY,
    TT_CALL,
    TT_TYPE_CONV,

    // Control flow
    TT_IF,
    TT_BLOCK,
    TT_LOOP,
} TtNodeKind;

// ── TtNode ─────────────────────────────────────────────────────────────

/**
 * Typed Tree node — a tagged union covering every desugared construct.
 * All nodes embed kind, type, and location as a common header.
 * The type field is never NULL; use TYPE_UNIT for non-expression nodes
 * and TYPE_ERROR for poison.
 */
struct TtNode {
    TtNodeKind kind;
    const Type *type;
    SourceLocation location;

    union {
        // TT_FILE
        struct {
            TtNode **declarations; /* buf */
        } file;

        // TT_MODULE
        struct {
            const char *name;
        } module;

        // TT_TYPE_ALIAS
        struct {
            const char *name;
            bool is_public;
            const Type *underlying;
        } type_alias;

        // TT_FUNCTION_DECL
        struct {
            const char *name;
            bool is_public;
            TtSymbol *symbol;
            TtNode **params; /* buf */
            const Type *return_type;
            TtNode *body;
        } function_decl;

        // TT_PARAM
        struct {
            TtSymbol *symbol;
            const char *name;
            const Type *param_type;
        } param;

        // TT_VAR_DECL
        struct {
            TtSymbol *symbol;
            const char *name;
            const Type *var_type;
            TtNode *initializer;
            bool is_mut;
        } var_decl;

        // TT_RETURN
        struct {
            TtNode *value;
        } return_stmt;

        // TT_ASSIGN
        struct {
            TtNode *target;
            TtNode *value;
        } assign;

        // TT_BREAK
        struct {
            TtNode *value;
        } break_stmt;

        // TT_EXPR_STMT
        struct {
            TtNode *expression;
        } expr_stmt;

        // TT_BOOL_LIT
        struct {
            bool value;
        } bool_lit;

        // TT_INT_LIT
        struct {
            uint64_t value;
            TypeKind int_kind;
        } int_lit;

        // TT_FLOAT_LIT
        struct {
            double value;
            TypeKind float_kind;
        } float_lit;

        // TT_CHAR_LIT
        struct {
            uint32_t value;
        } char_lit;

        // TT_STRING_LIT
        struct {
            const char *value;
        } string_lit;

        // TT_ARRAY_LIT
        struct {
            TtNode **elements; /* buf */
        } array_lit;

        // TT_TUPLE_LIT
        struct {
            TtNode **elements; /* buf */
        } tuple_lit;

        // TT_VAR_REF
        struct {
            TtSymbol *symbol;
        } var_ref;

        // TT_MODULE_ACCESS
        struct {
            TtNode *object;
            const char *member;
        } module_access;

        // TT_INDEX
        struct {
            TtNode *object;
            TtNode *index;
        } index_access;

        // TT_TUPLE_INDEX
        struct {
            TtNode *object;
            int32_t element_index;
        } tuple_index;

        // TT_UNARY
        struct {
            TokenKind op;
            TtNode *operand;
        } unary;

        // TT_BINARY
        struct {
            TokenKind op;
            TtNode *left;
            TtNode *right;
        } binary;

        // TT_CALL
        struct {
            TtNode *callee;
            TtNode **arguments; /* buf */
        } call;

        // TT_TYPE_CONV
        struct {
            TtNode *operand;
            const Type *target_type;
        } type_conv;

        // TT_IF
        struct {
            TtNode *condition;
            TtNode *then_body;
            TtNode *else_body;
        } if_expr;

        // TT_BLOCK
        struct {
            TtNode **statements; /* buf */
            TtNode *result;
        } block;

        // TT_LOOP
        struct {
            TtNode *body;
        } loop;
    };
};

// ── TtNode constructors ───────────────────────────────────────────────

/** Allocate a zero-initialised TtNode from @p arena. */
TtNode *tt_new(Arena *arena, TtNodeKind kind, const Type *type, SourceLocation location);
/** Allocate a TtSymbol from @p arena. */
TtSymbol *tt_symbol_new(Arena *arena, TtSymbolKind kind, Symbol *sema_symbol, bool is_mut,
                        SourceLocation location);

// ── TT dump ───────────────────────────────────────────────────────────

/** Recursively pretty-print @p node to stderr at @p indent levels. */
void tt_dump(const TtNode *node, int32_t indent);

#endif // RG_TT_H
