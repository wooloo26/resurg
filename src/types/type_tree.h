#ifndef RG_TYPE_TREE_H
#define RG_TYPE_TREE_H

#include "core/common.h"
#include "core/token.h"
#include "sema/_sema.h"
#include "types/types.h"

/**
 * @file type_tree.h
 * @brief Typed Tree — fully typed, desugared intermediate representation.
 *
 * Every Typed Tree node carries a resolved type (never NULL), desugared constructs,
 * scope-resolved identifiers (TtSymbol wrapping Sema's Symbol), and no
 * syntactic sugar.
 */

typedef struct Symbol Symbol;
typedef struct TtNode TtNode;
typedef struct TtSymbol TtSymbol;

// ── TtSymbol ───────────────────────────────────────────────────────────

/** Symbol kind in the TT — authoritative for lowering and codegen. */
typedef enum {
    TT_SYMBOL_VARIABLE,
    TT_SYMBOL_PARAMETER,
    TT_SYMBOL_FUNCTION,
    TT_SYMBOL_TYPE,
    TT_SYMBOL_MODULE,
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
    TT_FUNCTION_DECLARATION,
    TT_PARAMETER,

    // Statements
    TT_VARIABLE_DECLARATION,
    TT_RETURN,
    TT_ASSIGN,
    TT_BREAK,
    TT_CONTINUE,
    TT_EXPRESSION_STATEMENT,

    // Literals
    TT_BOOL_LITERAL,
    TT_INT_LITERAL,
    TT_FLOAT_LITERAL,
    TT_CHAR_LITERAL,
    TT_STRING_LITERAL,
    TT_UNIT_LITERAL,
    TT_ARRAY_LITERAL,
    TT_TUPLE_LITERAL,

    // References
    TT_VARIABLE_REFERENCE,
    TT_MODULE_ACCESS,
    TT_INDEX,
    TT_TUPLE_INDEX,

    // Operations
    TT_UNARY,
    TT_BINARY,
    TT_CALL,
    TT_TYPE_CONVERSION,

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

        // TT_FUNCTION_DECLARATION
        struct {
            const char *name;
            bool is_public;
            TtSymbol *symbol;
            TtNode **params; /* buf */
            const Type *return_type;
            TtNode *body;
        } function_declaration;

        // TT_PARAMETER
        struct {
            TtSymbol *symbol;
            const char *name;
            const Type *param_type;
        } parameter;

        // TT_VARIABLE_DECLARATION
        struct {
            TtSymbol *symbol;
            const char *name;
            const Type *var_type;
            TtNode *initializer;
            bool is_mut;
        } variable_declaration;

        // TT_RETURN
        struct {
            TtNode *value;
        } return_statement;

        // TT_ASSIGN
        struct {
            TtNode *target;
            TtNode *value;
        } assign;

        // TT_BREAK
        struct {
            TtNode *value;
        } break_statement;

        // TT_EXPRESSION_STATEMENT
        struct {
            TtNode *expression;
        } expression_statement;

        // TT_BOOL_LITERAL
        struct {
            bool value;
        } bool_literal;

        // TT_INT_LITERAL
        struct {
            uint64_t value;
            TypeKind int_kind;
        } int_literal;

        // TT_FLOAT_LITERAL
        struct {
            double value;
            TypeKind float_kind;
        } float_literal;

        // TT_CHAR_LITERAL
        struct {
            uint32_t value;
        } char_literal;

        // TT_STRING_LITERAL
        struct {
            const char *value;
        } string_literal;

        // TT_ARRAY_LITERAL
        struct {
            TtNode **elements; /* buf */
        } array_literal;

        // TT_TUPLE_LITERAL
        struct {
            TtNode **elements; /* buf */
        } tuple_literal;

        // TT_VARIABLE_REFERENCE
        struct {
            TtSymbol *symbol;
        } variable_reference;

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

        // TT_TYPE_CONVERSION
        struct {
            TtNode *operand;
            const Type *target_type;
        } type_conversion;

        // TT_IF
        struct {
            TtNode *condition;
            TtNode *then_body;
            TtNode *else_body;
        } if_expression;

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

#endif // RG_TYPE_TREE_H
