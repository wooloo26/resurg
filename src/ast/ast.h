#ifndef RG_AST_H
#define RG_AST_H

#include "core/common.h"
#include "core/token.h"
#include "types/types.h"

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
    AST_TYPE_SLICE,    // []T
    AST_TYPE_TUPLE,    // (A, B, ...)
    AST_TYPE_PTR,      // *T
} ASTTypeKind;

struct ASTType {
    ASTTypeKind kind;
    const char *name; // may be NULL (for NAME kind)
    SourceLoc loc;
    // AST_TYPE_ARRAY fields
    ASTType *array_elem; // heap-allocated elem type
    int32_t array_size;  // elem count N
    // AST_TYPE_SLICE fields
    ASTType *slice_elem; // heap-allocated elem type
    // AST_TYPE_TUPLE fields
    ASTType **tuple_elems; /* buf */
    // AST_TYPE_PTR fields
    ASTType *ptr_elem; // pointee type
};

/** A field def in a struct decl. */
typedef struct {
    const char *name;
    ASTType type;
    ASTNode *default_value; // may be NULL
} ASTStructField;

/** Variant kind in an enum decl. */
typedef enum {
    VARIANT_UNIT,   // Quit
    VARIANT_TUPLE,  // Write(str)
    VARIANT_STRUCT, // Move { x: i32, y: i32 }
} ASTVariantKind;

/** A variant in an enum decl. */
typedef struct {
    const char *name;
    ASTVariantKind kind;
    ASTType *tuple_types;   /* buf - for VARIANT_TUPLE */
    ASTStructField *fields; /* buf - for VARIANT_STRUCT */
    ASTNode *discriminant;  // explicit value (may be NULL)
} ASTEnumVariant;

/** Pattern kind in a match arm. */
typedef enum {
    PATTERN_WILDCARD,       // _
    PATTERN_BINDING,        // name
    PATTERN_LIT,            // 42, "hello", true
    PATTERN_RANGE,          // 1..10 or 1..=10
    PATTERN_VARIANT_UNIT,   // Quit
    PATTERN_VARIANT_TUPLE,  // Write(text)
    PATTERN_VARIANT_STRUCT, // Move { x, y }
} ASTPatternKind;

/** A pattern in a match arm. */
typedef struct ASTPattern ASTPattern;
struct ASTPattern {
    ASTPatternKind kind;
    SourceLoc loc;
    const char *name;          // binding name or variant name
    ASTNode *lit;              // for PATTERN_LIT
    ASTNode *range_start;      // for PATTERN_RANGE
    ASTNode *range_end;        // for PATTERN_RANGE
    bool range_inclusive;      // true for ..=
    ASTPattern **sub_patterns; /* buf - for tuple/struct variant */
    const char **field_names;  /* buf - for struct variant */
};

/** A single arm in a match expr. */
typedef struct {
    ASTPattern *pattern;
    ASTNode *guard; // may be NULL
    ASTNode *body;
} ASTMatchArm;

/** Discriminator for the ASTNode tagged union. */
typedef enum {
    // Top-level
    NODE_MODULE,     // module decl
    NODE_FILE,       // root list of decls
    NODE_TYPE_ALIAS, // type alias decl

    // Declarations
    NODE_FN_DECL,     // fn decl
    NODE_VAR_DECL,    // var decl (:= or var)
    NODE_PARAM,       // fn param
    NODE_STRUCT_DECL, // struct def
    NODE_ENUM_DECL,   // enum def
    NODE_PACT_DECL,   // pact def
    NODE_RETURN,      // return expr
    NODE_DEFER,       // defer { ... }

    // Statements
    NODE_EXPR_STMT, // expr used as stmt
    NODE_BREAK,     // break
    NODE_CONTINUE,  // continue

    // Expressions
    NODE_LIT,                // int, float, str, bool, char, unit lits
    NODE_ID,                 // var / fn ref
    NODE_UNARY,              // !x, -x
    NODE_BINARY,             // x + y, x == y, x && y
    NODE_ASSIGN,             // x = expr
    NODE_COMPOUND_ASSIGN,    // x += expr, x -= expr, ...
    NODE_CALL,               // foo(a, b)
    NODE_MEMBER,             // module.func (dot access)
    NODE_IDX,                // arr[i] (array idxing)
    NODE_IF,                 // if/else expr
    NODE_LOOP,               // loop { ... }
    NODE_WHILE,              // while cond { ... }
    NODE_FOR,                // for i := 0..N { ... }
    NODE_BLOCK,              // { stmts; optional trailing expr }
    NODE_STR_INTERPOLATION,  // "hello {name}, {1+2}"
    NODE_ARRAY_LIT,          // [1, 2, 3] or [3]i32{1, 2, 3}
    NODE_SLICE_LIT,          // []i32{1, 2, 3}
    NODE_SLICE_EXPR,         // arr[..], s[1..4], s[2..], s[..3]
    NODE_TUPLE_LIT,          // (1, true, "hi")
    NODE_TYPE_CONVERSION,    // i64(100), f32(3.14)
    NODE_STRUCT_LIT,         // Point { x = 1.0, y = 2.0 }
    NODE_STRUCT_DESTRUCTURE, // {x, y} := expr
    NODE_TUPLE_DESTRUCTURE,  // (a, b) := expr
    NODE_ADDRESS_OF,         // &expr (heap alloc or address-of)
    NODE_DEREF,              // *expr (ptr deref)
    NODE_MATCH,              // match expr { arms }
    NODE_ENUM_INIT,          // Enum.Variant or Enum.Variant(args) or Enum.Variant { fields }
} NodeKind;

/** Sub-kind for NODE_LIT - indicates which payload field is active. */
typedef enum {
    LIT_BOOL,
    LIT_I8,
    LIT_I16,
    LIT_I32,
    LIT_I64,
    LIT_I128,
    LIT_U8,
    LIT_U16,
    LIT_U32,
    LIT_U64,
    LIT_U128,
    LIT_ISIZE,
    LIT_USIZE,
    LIT_F32,
    LIT_F64,
    LIT_CHAR,
    LIT_STR,
    LIT_UNIT,
} LitKind;

/**
 * AST node - a tagged union covering every syntactic construct.
 * Each variant stores its children in the anonymous union; stretchy-buf
 * ptrs are marked with a trailing @c buf comment.
 */
struct ASTNode {
    NodeKind kind;
    SourceLoc loc;
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

        // NODE_TYPE_ALIAS
        struct {
            const char *name;
            ASTType alias_type;
        } type_alias;

        // NODE_FN_DECL
        struct {
            bool is_pub;
            const char *name;
            ASTNode **params; /* buf */
            ASTType return_type;
            ASTNode *body;
            // Method-specific fields (NULL / false for regular fns)
            const char *recv_name;
            bool is_mut_recv;
            bool is_ptr_recv;
            const char *owner_struct;
        } fn_decl;

        // NODE_PARAM
        struct {
            const char *name;
            ASTType type;
            bool is_mut; // true for `mut name: *T`
        } param;

        // NODE_VAR_DECL
        struct {
            const char *name;
            ASTType type;  // may be AST_TYPE_INFERRED
            ASTNode *init; // init expr
            bool is_var;   // true for `var x: T = ...`, false for `:=`
            bool is_immut; // true for `immut x := ...`
        } var_decl;

        // NODE_EXPR_STMT
        struct {
            ASTNode *expr;
        } expr_stmt;

        // NODE_LIT
        struct {
            LitKind kind;
            union {
                bool boolean_value;
                uint64_t integer_value; // for all integer lit kinds
                double float64_value;   // for LIT_F32 and LIT_F64
                const char *str_value;
                uint32_t char_value; // Unicode scalar value (for LIT_CHAR)
            };
        } lit;

        // NODE_ID
        struct {
            const char *name;
        } id;

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
            ASTNode **args;         /* buf */
            const char **arg_names; /* buf - NULL entry = posal */
            bool *arg_is_mut;       /* buf - true = `mut` at call site */
        } call;

        // NODE_MEMBER
        struct {
            ASTNode *object;
            const char *member;
        } member;

        // NODE_IDX
        struct {
            ASTNode *object;
            ASTNode *idx;
        } idx_access;

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
            const char *idx_name; // for |v, i| form (may be NULL)
            ASTNode *start;       // range start expr (NULL for slice iteration)
            ASTNode *end;         // range end expr (NULL for slice iteration)
            ASTNode *iterable;    // slice expr (NULL for range iteration)
            ASTNode *body;
        } for_loop;

        // NODE_BLOCK
        struct {
            ASTNode **stmts; /* buf */
            ASTNode *result; // may be NULL
        } block;

        // NODE_STR_INTERPOLATION
        struct {
            ASTNode **parts; /* buf */
        } str_interpolation;

        // NODE_ARRAY_LIT
        struct {
            ASTNode **elems;   /* buf */
            ASTType elem_type; // may be inferred
            int32_t size;      // N from [N]T prefix, or elem count
        } array_lit;

        // NODE_SLICE_LIT
        struct {
            ASTNode **elems;   /* buf */
            ASTType elem_type; // from []T prefix
        } slice_lit;

        // NODE_SLICE_EXPR
        struct {
            ASTNode *object; // the array/slice being sliced
            ASTNode *start;  // may be NULL (s[..3])
            ASTNode *end;    // may be NULL (s[2..])
            bool full_range; // true for arr[..]
        } slice_expr;

        // NODE_TUPLE_LIT
        struct {
            ASTNode **elems; /* buf */
        } tuple_lit;

        // NODE_TYPE_CONVERSION
        struct {
            ASTType target_type;
            ASTNode *operand;
        } type_conversion;

        // NODE_STRUCT_DECL
        struct {
            const char *name;
            ASTStructField *fields;    /* buf */
            ASTNode **methods;         /* buf - NODE_FN_DECL */
            const char **embedded;     /* buf - embedded struct names */
            const char **conformances; /* buf - pact names */
        } struct_decl;

        // NODE_STRUCT_LIT
        struct {
            const char *name;
            const char **field_names; /* buf */
            ASTNode **field_values;   /* buf */
        } struct_lit;

        // NODE_STRUCT_DESTRUCTURE
        struct {
            const char **field_names; /* buf */
            const char **aliases;     /* buf - NULL entry = use field name */
            ASTNode *value;
        } struct_destructure;

        // NODE_TUPLE_DESTRUCTURE
        struct {
            const char **names; /* buf */
            ASTNode *value;
            bool has_rest;    // true when `..` appears in pattern
            int32_t rest_pos; // idx of `..` in pattern (-1 if none)
        } tuple_destructure;

        // NODE_ADDRESS_OF
        struct {
            ASTNode *operand;
        } address_of;

        // NODE_DEREF
        struct {
            ASTNode *operand;
        } deref;

        // NODE_ENUM_DECL
        struct {
            const char *name;
            ASTEnumVariant *variants; /* buf */
            ASTNode **methods;        /* buf - NODE_FN_DECL */
        } enum_decl;

        // NODE_MATCH
        struct {
            ASTNode *operand;
            ASTMatchArm *arms; /* buf */
        } match_expr;

        // NODE_ENUM_INIT
        struct {
            const char *enum_name;
            const char *variant_name;
            ASTNode **args;           /* buf - for tuple variants */
            const char **field_names; /* buf - for struct variants */
            ASTNode **field_values;   /* buf - for struct variants */
        } enum_init;

        // NODE_PACT_DECL
        struct {
            const char *name;
            ASTStructField *fields;   /* buf - required fields */
            ASTNode **methods;        /* buf - required + default methods */
            const char **super_pacts; /* buf - constraint alias pact names */
        } pact_decl;

        // NODE_RETURN
        struct {
            ASTNode *value; // may be NULL
        } return_stmt;

        // NODE_WHILE
        struct {
            ASTNode *cond;
            ASTNode *body;
        } while_loop;

        // NODE_DEFER
        struct {
            ASTNode *body; // block body
        } defer_stmt;

        // NODE_BREAK
        struct {
            ASTNode *value; // may be NULL (break with value for loop exprs)
        } break_stmt;
    };
};

/** Allocate a zero-initialised ASTNode of the given @p kind from @p arena. */
ASTNode *ast_new(Arena *arena, NodeKind kind, SourceLoc loc);

/**
 * Recursively pretty-print @p node to stderr (indented by @p indent
 * levels).  Used by --dump-ast.
 */
void ast_dump(const ASTNode *node, int32_t indent);

#endif // RG_AST_H
