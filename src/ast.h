#ifndef RG_AST_H
#define RG_AST_H

#include "common.h"
#include "token.h"
#include "types.h"

/**
 * @file ast.h
 * @brief Abstract Syntax Tree - node kinds, the ASTNode tagged union, and
 * construction / debug-dump utilities.
 */

typedef struct ASTNode ASTNode;
typedef struct ASTType ASTType;

/**
 * Syntactic type annotation - either an explicit name, an array type,
 * a tuple type, or "inferred".
 */
typedef enum {
    AST_TYPE_NAME,     // bool, i32, u32, f64, str, unit, user-defined
    AST_TYPE_INFERRED, // type omitted, to be inferred by semantic analysis
    AST_TYPE_ARRAY,    // [N]T
    AST_TYPE_TUPLE,    // (A, B, ...)
} ASTTypeKind;

struct ASTType {
    ASTTypeKind kind;
    const char *name; // may be NULL (for NAME kind)
    SourceLocation location;
    // AST_TYPE_ARRAY fields
    ASTType *array_element; // heap-allocated element type
    int32_t array_size;     // element count N
    // AST_TYPE_TUPLE fields
    ASTType **tuple_elements; /* buf */
};

/** Discriminator for the ASTNode tagged union. */
typedef enum {
    // Top-level
    NODE_MODULE,     // module declaration
    NODE_FILE,       // root list of declarations
    NODE_TYPE_ALIAS, // type alias declaration

    // Declarations
    NODE_FUNCTION_DECLARATION, // function declaration
    NODE_VARIABLE_DECLARATION, // variable declaration (:= or var)
    NODE_PARAMETER,            // function parameter

    // Statements
    NODE_EXPRESSION_STATEMENT, // expression used as statement
    NODE_BREAK,                // break
    NODE_CONTINUE,             // continue

    // Expressions
    NODE_LITERAL,              // int, float, str, bool, char, unit literals
    NODE_IDENTIFIER,           // variable / function reference
    NODE_UNARY,                // !x, -x
    NODE_BINARY,               // x + y, x == y, x && y
    NODE_ASSIGN,               // x = expr
    NODE_COMPOUND_ASSIGN,      // x += expr, x -= expr, ...
    NODE_CALL,                 // foo(a, b)
    NODE_MEMBER,               // module.func (dot access)
    NODE_INDEX,                // arr[i] (array indexing)
    NODE_IF,                   // if/else expression
    NODE_LOOP,                 // loop { ... }
    NODE_FOR,                  // for i := 0..N { ... }
    NODE_BLOCK,                // { stmts; optional trailing expr }
    NODE_STRING_INTERPOLATION, // "hello {name}, {1+2}"
    NODE_ARRAY_LITERAL,        // [1, 2, 3] or [3]i32[1, 2, 3]
    NODE_TUPLE_LITERAL,        // (1, true, "hi")
    NODE_TYPE_CONVERSION,      // i64(100), f32(3.14)
} NodeKind;

/** Sub-kind for NODE_LITERAL - indicates which payload field is active. */
typedef enum {
    LITERAL_BOOL,
    LITERAL_I8,
    LITERAL_I16,
    LITERAL_I32,
    LITERAL_I64,
    LITERAL_I128,
    LITERAL_U8,
    LITERAL_U16,
    LITERAL_U32,
    LITERAL_U64,
    LITERAL_U128,
    LITERAL_ISIZE,
    LITERAL_USIZE,
    LITERAL_F32,
    LITERAL_F64,
    LITERAL_CHAR,
    LITERAL_STRING,
    LITERAL_UNIT,
} LiteralKind;

/**
 * AST node - a tagged union covering every syntactic construct.
 * Each variant stores its children in the anonymous union; stretchy-buffer
 * pointers are marked with a trailing @c buf comment.
 */
struct ASTNode {
    NodeKind kind;
    SourceLocation location;
    const Type *type; // may be NULL

    union {
        // NODE_MODULE
        struct {
            const char *name;
        } module;

        // NODE_FILE
        struct {
            ASTNode **declarations; /* buf */
        } file;

        // NODE_TYPE_ALIAS
        struct {
            const char *name;
            ASTType alias_type;
        } type_alias;

        // NODE_FUNCTION_DECLARATION
        struct {
            bool is_public;
            const char *name;
            ASTNode **parameters; /* buf */
            ASTType return_type;
            ASTNode *body;
        } function_declaration;

        // NODE_PARAMETER
        struct {
            const char *name;
            ASTType type;
        } parameter;

        // NODE_VARIABLE_DECLARATION
        struct {
            const char *name;
            ASTType type;         // may be AST_TYPE_INFERRED
            ASTNode *initializer; // initializer expression
            bool is_variable;     // true for `var x: T = ...`, false for `:=`
        } variable_declaration;

        // NODE_EXPRESSION_STATEMENT
        struct {
            ASTNode *expression;
        } expression_statement;

        // NODE_LITERAL
        struct {
            LiteralKind kind;
            union {
                bool boolean_value;
                uint64_t integer_value; // for all integer literal kinds
                double float64_value;   // for LITERAL_F32 and LITERAL_F64
                const char *string_value;
                char char_value; // for LITERAL_CHAR
            };
        } literal;

        // NODE_IDENTIFIER
        struct {
            const char *name;
        } identifier;

        // NODE_UNARY
        struct {
            TokenKind op; // TOKEN_MINUS, TOKEN_BANG
            ASTNode *operand;
        } unary;

        // NODE_BINARY
        struct {
            TokenKind op;
            ASTNode *left;
            ASTNode *right;
        } binary;

        // NODE_ASSIGN
        struct {
            ASTNode *target;
            ASTNode *value;
        } assign;

        // NODE_COMPOUND_ASSIGN
        struct {
            TokenKind op; // TOKEN_PLUS_EQUAL, TOKEN_MINUS_EQUAL, etc.
            ASTNode *target;
            ASTNode *value;
        } compound_assign;

        // NODE_CALL
        struct {
            ASTNode *callee;
            ASTNode **arguments; /* buf */
        } call;

        // NODE_MEMBER
        struct {
            ASTNode *object;
            const char *member;
        } member;

        // NODE_INDEX
        struct {
            ASTNode *object;
            ASTNode *index;
        } index_access;

        // NODE_IF
        struct {
            ASTNode *condition;
            ASTNode *then_body;
            ASTNode *else_body; // may be NULL
        } if_expression;

        // NODE_LOOP
        struct {
            ASTNode *body;
        } loop;

        // NODE_FOR
        struct {
            const char *variable_name;
            ASTNode *start; // range start expression
            ASTNode *end;   // range end expression
            ASTNode *body;
        } for_loop;

        // NODE_BLOCK
        struct {
            ASTNode **statements; /* buf */
            ASTNode *result;      // may be NULL
        } block;

        // NODE_STRING_INTERPOLATION
        struct {
            ASTNode **parts; /* buf */
        } string_interpolation;

        // NODE_ARRAY_LITERAL
        struct {
            ASTNode **elements;   /* buf */
            ASTType element_type; // may be inferred
            int32_t size;         // N from [N]T prefix, or element count
        } array_literal;

        // NODE_TUPLE_LITERAL
        struct {
            ASTNode **elements; /* buf */
        } tuple_literal;

        // NODE_TYPE_CONVERSION
        struct {
            ASTType target_type;
            ASTNode *operand;
        } type_conversion;
    };
};

/** Allocate a zero-initialised ASTNode of the given @p kind from @p arena. */
ASTNode *ast_new(Arena *arena, NodeKind kind, SourceLocation location);

/**
 * Recursively pretty-print @p node to stderr (indented by @p indent
 * levels).  Used by --dump-ast.
 */
void ast_dump(const ASTNode *node, int32_t indent);

#endif // RG_AST_H
