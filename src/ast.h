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
 * Syntactic type annotation - either an explicit name or "inferred".
 * Resolved types (used during sema and codegen) live in types.h.
 */
typedef enum {
    AST_TYPE_NAME,     // bool, i32, u32, f64, str, unit - or user-defined
    AST_TYPE_INFERRED, // type omitted, to be inferred by semantic analysis
} ASTTypeKind;

struct ASTType {
    ASTTypeKind kind;
    const char *name; // may be NULL
    SourceLocation location;
};

/** Discriminator for the ASTNode tagged union. */
typedef enum {
    // Top-level
    NODE_MODULE, // module declaration
    NODE_FILE,   // root list of declarations

    // Declarations
    NODE_FUNCTION_DECLARATION, // function declaration
    NODE_VARIABLE_DECLARATION, // variable declaration (:= or var)
    NODE_PARAMETER,            // function parameter

    // Statements
    NODE_EXPRESSION_STATEMENT, // expression used as statement
    NODE_BREAK,                // break
    NODE_CONTINUE,             // continue

    // Expressions
    NODE_LITERAL,              // int, float, str, bool, unit literals
    NODE_IDENTIFIER,           // variable / function reference
    NODE_UNARY,                // !x, -x
    NODE_BINARY,               // x + y, x == y, x && y
    NODE_ASSIGN,               // x = expr
    NODE_COMPOUND_ASSIGN,      // x += expr, x -= expr, ...
    NODE_CALL,                 // foo(a, b)
    NODE_MEMBER,               // module.func (dot access)
    NODE_IF,                   // if/else expression
    NODE_LOOP,                 // loop { ... }
    NODE_FOR,                  // for i := 0..N { ... }
    NODE_BLOCK,                // { stmts; optional trailing expr }
    NODE_STRING_INTERPOLATION, // "hello {name}, {1+2}"
} NodeKind;

/** Sub-kind for NODE_LITERAL - indicates which payload field is active. */
typedef enum {
    LITERAL_BOOL,
    LITERAL_I32,
    LITERAL_U32,
    LITERAL_F64,
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
                int64_t integer_value; // for LITERAL_I32 and LITERAL_U32
                double float64_value;
                const char *string_value;
            };
        } literal;

        // NODE_IDENTIFIER
        struct {
            const char *name;
        } identifier;

        // NODE_UNARY
        struct {
            TokenKind operator; // TOKEN_MINUS, TOKEN_BANG
            ASTNode *operand;
        } unary;

        // NODE_BINARY
        struct {
            TokenKind operator;
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
            TokenKind operator; // TOKEN_PLUS_EQUAL, TOKEN_MINUS_EQUAL, etc.
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
