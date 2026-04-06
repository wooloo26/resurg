#ifndef RG_TYPE_TREE_H
#define RG_TYPE_TREE_H

#include "core/common.h"
#include "core/token.h"
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
    TT_DEFER,

    // Literals
    TT_BOOL_LITERAL,
    TT_INT_LITERAL,
    TT_FLOAT_LITERAL,
    TT_CHAR_LITERAL,
    TT_STRING_LITERAL,
    TT_UNIT_LITERAL,
    TT_ARRAY_LITERAL,
    TT_SLICE_LITERAL,
    TT_TUPLE_LITERAL,

    // References
    TT_VARIABLE_REFERENCE,
    TT_MODULE_ACCESS,
    TT_INDEX,
    TT_SLICE_EXPR,
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

    // Struct-related
    TT_STRUCT_DECLARATION,
    TT_STRUCT_LITERAL,
    TT_STRUCT_FIELD_ACCESS,
    TT_METHOD_CALL,

    // Pointer-related
    TT_HEAP_ALLOC,
    TT_ADDRESS_OF,
    TT_DEREF,

    // Enum-related
    TT_ENUM_DECLARATION,
    TT_MATCH,
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
            TtNode **declarations;       /* buf */
            const Type **compound_types; /* buf */
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
            bool is_receiver;
            bool is_mut_receiver;
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

        // TT_SLICE_LITERAL
        struct {
            TtNode **elements; /* buf */
        } slice_literal;

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

        // TT_SLICE_EXPR
        struct {
            TtNode *object;
            TtNode *start;   // may be NULL
            TtNode *end;     // may be NULL
            bool from_array; // true when source is an array (copy semantics)
        } slice_expr;

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

        // TT_DEFER
        struct {
            TtNode *body;
        } defer_statement;

        // TT_STRUCT_DECLARATION
        struct {
            const char *name;
            const Type *struct_type;
        } struct_decl;

        // TT_STRUCT_LITERAL
        struct {
            const char **field_names; /* buf */
            TtNode **field_values;    /* buf */
        } struct_literal;

        // TT_STRUCT_FIELD_ACCESS
        struct {
            TtNode *object;
            const char *field;
            bool via_pointer;
        } struct_field_access;

        // TT_METHOD_CALL
        struct {
            TtNode *receiver;
            const char *mangled_name;
            TtNode **arguments; /* buf */
        } method_call;

        // TT_HEAP_ALLOC
        struct {
            TtNode *operand;
        } heap_alloc;

        // TT_ADDRESS_OF
        struct {
            TtNode *operand;
        } address_of;

        // TT_DEREF
        struct {
            TtNode *operand;
        } deref;

        // TT_ENUM_DECLARATION
        struct {
            const char *name;
            const Type *enum_type;
        } enum_decl;

        // TT_MATCH
        struct {
            TtNode *operand;
            TtNode **arm_conditions; /* buf - pattern condition per arm */
            TtNode **arm_guards;     /* buf - guard expression per arm (NULL if none) */
            TtNode **arm_bodies;     /* buf - body expression per arm */
            TtNode **arm_bindings;   /* buf - block of binding stmts (NULL if none) */
        } match_expr;
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

// ── TT child visitor ──────────────────────────────────────────────────

/** Callback invoked for each child pointer of a TtNode. */
typedef void (*TtChildVisitor)(void *context, TtNode **child_ptr);
/** Call @p visitor for every child pointer in @p node. */
void tt_visit_children(TtNode *node, TtChildVisitor visitor, void *context);

#endif // RG_TYPE_TREE_H
