#ifndef RG_AST_H
#define RG_AST_H

#include "common.h"
#include "token.h"
#include "types.h"

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
typedef struct ASTNode ASTNode;
typedef struct ASTType ASTType;

// ---------------------------------------------------------------------------
// Types (syntactic representation — resolved types live in types.h)
// ---------------------------------------------------------------------------
typedef enum {
    AST_TYPE_NAME,     // bool, i32, u32, f64, str, unit — or user-defined
    AST_TYPE_INFERRED, // type omitted, to be inferred by sema
} ASTTypeKind;

struct ASTType {
    ASTTypeKind kind;
    const char *name; // may be NULL
    SrcLoc loc;
};

// ---------------------------------------------------------------------------
// AST node kinds
// ---------------------------------------------------------------------------
typedef enum {
    // Top-level
    NODE_MODULE, // module declaration
    NODE_FILE,   // root list of declarations

    // Declarations
    NODE_FN_DECL,  // function declaration
    NODE_VAR_DECL, // variable declaration (:= or var)
    NODE_PARAM,    // function parameter

    // Statements
    NODE_EXPR_STMT, // expression used as statement
    NODE_ASSERT,    // assert(expr) or assert(expr, "msg")
    NODE_BREAK,     // break
    NODE_CONTINUE,  // continue

    // Expressions
    NODE_LITERAL,         // int, float, str, bool, unit literals
    NODE_IDENT,           // variable / function reference
    NODE_UNARY,           // !x, -x
    NODE_BINARY,          // x + y, x == y, x && y
    NODE_ASSIGN,          // x = expr
    NODE_COMPOUND_ASSIGN, // x += expr, x -= expr, ...
    NODE_CALL,            // foo(a, b)
    NODE_MEMBER,          // module.func (dot access)
    NODE_IF,              // if/else expression
    NODE_LOOP,            // loop { ... }
    NODE_FOR,             // for i := 0..N { ... }
    NODE_BLOCK,           // { stmts; optional trailing expr }
    NODE_STR_INTERP,      // "hello {name}, {1+2}"
} NodeKind;

// ---------------------------------------------------------------------------
// Literal kind (subset of token kinds, for NODE_LITERAL)
// ---------------------------------------------------------------------------
typedef enum {
    LIT_BOOL,
    LIT_I32,
    LIT_U32,
    LIT_F64,
    LIT_STR,
    LIT_UNIT,
} LitKind;

// ---------------------------------------------------------------------------
// AST node — tagged union
// ---------------------------------------------------------------------------
struct ASTNode {
    NodeKind kind;
    SrcLoc loc;
    const Type *type; // may be NULL

    union {
        // NODE_MODULE
        struct {
            const char *name;
        } module;

        // NODE_FILE
        struct {
            ASTNode **decls; /* buf */
        } file;

        // NODE_FN_DECL
        struct {
            bool is_pub;
            const char *name;
            ASTNode **params; /* buf */
            ASTType return_type;
            ASTNode *body;
        } fn_decl;

        // NODE_PARAM
        struct {
            const char *name;
            ASTType type;
        } param;

        // NODE_VAR_DECL
        struct {
            const char *name;
            ASTType type;  // may be AST_TYPE_INFERRED
            ASTNode *init; // initializer expression
            bool is_var;   // true for `var x: T = ...`, false for `:=`
        } var_decl;

        // NODE_EXPR_STMT
        struct {
            ASTNode *expr;
        } expr_stmt;

        // NODE_ASSERT
        struct {
            ASTNode *cond;
            ASTNode *message; // may be NULL
        } assert_stmt;

        // NODE_LITERAL
        struct {
            LitKind kind;
            union {
                bool bool_val;
                int64_t int_val; // for LIT_I32 and LIT_U32
                double f64_val;
                const char *str_val;
            };
        } literal;

        // NODE_IDENT
        struct {
            const char *name;
        } ident;

        // NODE_UNARY
        struct {
            TokenKind op; // TOK_MINUS, TOK_BANG
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
            TokenKind op; // TOK_PLUS_EQ, TOK_MINUS_EQ, etc.
            ASTNode *target;
            ASTNode *value;
        } compound_assign;

        // NODE_CALL
        struct {
            ASTNode *callee;
            ASTNode **args; /* buf */
        } call;

        // NODE_MEMBER
        struct {
            ASTNode *object;
            const char *member;
        } member;

        // NODE_IF
        struct {
            ASTNode *cond;
            ASTNode *then_body;
            ASTNode *else_body; // may be NULL
        } if_expr;

        // NODE_LOOP
        struct {
            ASTNode *body;
        } loop;

        // NODE_FOR
        struct {
            const char *var_name;
            ASTNode *start; // range start expression
            ASTNode *end;   // range end expression
            ASTNode *body;
        } for_loop;

        // NODE_BLOCK
        struct {
            ASTNode **stmts; /* buf */
            ASTNode *result; // may be NULL
        } block;

        // NODE_STR_INTERP
        struct {
            ASTNode **parts; /* buf */
        } str_interp;
    };
};

// ---------------------------------------------------------------------------
// AST constructors (allocate from arena)
// ---------------------------------------------------------------------------
// Allocate and initialize an AST node of the given kind.
ASTNode *ast_new(Arena *a, NodeKind kind, SrcLoc loc);

// Print the AST tree to stderr for debugging.
void ast_dump(const ASTNode *node, int32_t indent);

#endif // RG_AST_H
