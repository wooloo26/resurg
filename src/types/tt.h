#ifndef RG_TYPE_TREE_H
#define RG_TYPE_TREE_H

#include "core/common.h"
#include "core/token.h"
#include "types/types.h"

/**
 * @file tt.h
 * @brief Typed Tree — fully typed, desugared intermediate representation.
 *
 * Every Typed Tree node carries a resolved type (never NULL), desugared constructs,
 * scope-resolved ids (TTSym wrapping Sema's Sym), and no
 * syntactic sugar.
 */

typedef struct Sym Sym;
typedef struct TTNode TTNode;
typedef struct TTSym TTSym;

// ── TTSym ───────────────────────────────────────────────────────────

/** Sym kind in the TT — authoritative for lowering and codegen. */
typedef enum {
    TT_SYM_VAR,
    TT_SYM_PARAM,
    TT_SYM_FN,
    TT_SYM_TYPE,
    TT_SYM_MODULE,
} TtSymKind;

/**
 * TT sym — wraps Sema's Sym with additional lowering metadata.
 * TTSym.kind is authoritative in TT/codegen; accessors delegate to
 * Sema for name and type.
 */
struct TTSym {
    TtSymKind kind;
    Sym *sema_sym;
    bool is_mut;
    bool is_ptr_recv; // for method syms: recv is *T
    const char *mangled_name;
    SourceLoc loc;
};

/** Return the name of a TTSym (delegates to sema_sym). */
const char *tt_sym_name(const TTSym *sym);
/** Return the type of a TTSym (delegates to sema_sym). */
const Type *tt_sym_type(const TTSym *sym);

// ── TtNodeKind ─────────────────────────────────────────────────────────

/** Discriminator for TTNode tagged union. */
typedef enum {
    // Top-level
    TT_FILE,
    TT_MODULE,
    TT_TYPE_ALIAS,

    // Declarations
    TT_FN_DECL,
    TT_PARAM,

    // Statements
    TT_VAR_DECL,
    TT_RETURN,
    TT_ASSIGN,
    TT_BREAK,
    TT_CONTINUE,
    TT_DEFER,

    // Lits
    TT_BOOL_LIT,
    TT_INT_LIT,
    TT_FLOAT_LIT,
    TT_CHAR_LIT,
    TT_STR_LIT,
    TT_UNIT_LIT,
    TT_ARRAY_LIT,
    TT_SLICE_LIT,
    TT_TUPLE_LIT,

    // References
    TT_VAR_REF,
    TT_MODULE_ACCESS,
    TT_IDX,
    TT_SLICE_EXPR,
    TT_TUPLE_IDX,

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
    TT_STRUCT_DECL,
    TT_STRUCT_LIT,
    TT_STRUCT_FIELD_ACCESS,
    TT_METHOD_CALL,

    // Pointer-related
    TT_HEAP_ALLOC,
    TT_ADDRESS_OF,
    TT_DEREF,

    // Enum-related
    TT_ENUM_DECL,
    TT_MATCH,
} TtNodeKind;

// ── TTNode ─────────────────────────────────────────────────────────────

/**
 * Typed Tree node — a tagged union covering every desugared construct.
 * All nodes embed kind, type, and loc as a common header.
 * The type field is never NULL; use TYPE_UNIT for non-expr nodes
 * and TYPE_ERR for poison.
 */
struct TTNode {
    TtNodeKind kind;
    const Type *type;
    SourceLoc loc;

    union {
        // TT_FILE
        struct {
            TTNode **decls;              /* buf */
            const Type **compound_types; /* buf */
        } file;

        // TT_MODULE
        struct {
            const char *name;
        } module;

        // TT_TYPE_ALIAS
        struct {
            const char *name;
            bool is_pub;
            const Type *underlying;
        } type_alias;

        // TT_FN_DECL
        struct {
            const char *name;
            bool is_pub;
            TTSym *sym;
            TTNode **params; /* buf */
            const Type *return_type;
            TTNode *body;
        } fn_decl;

        // TT_PARAM
        struct {
            TTSym *sym;
            const char *name;
            const Type *param_type;
            bool is_recv;
            bool is_mut_recv;
            bool is_ptr_recv;
        } param;

        // TT_VAR_DECL
        struct {
            TTSym *sym;
            const char *name;
            const Type *var_type;
            TTNode *init;
            bool is_mut;
        } var_decl;

        // TT_RETURN
        struct {
            TTNode *value;
        } return_stmt;

        // TT_ASSIGN
        struct {
            TTNode *target;
            TTNode *value;
        } assign;

        // TT_BREAK
        struct {
            TTNode *value;
        } break_stmt;

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

        // TT_STR_LIT
        struct {
            const char *value;
        } str_lit;

        // TT_ARRAY_LIT
        struct {
            TTNode **elems; /* buf */
        } array_lit;

        // TT_SLICE_LIT
        struct {
            TTNode **elems; /* buf */
        } slice_lit;

        // TT_TUPLE_LIT
        struct {
            TTNode **elems; /* buf */
        } tuple_lit;

        // TT_VAR_REF
        struct {
            TTSym *sym;
        } var_ref;

        // TT_MODULE_ACCESS
        struct {
            TTNode *object;
            const char *member;
        } module_access;

        // TT_IDX
        struct {
            TTNode *object;
            TTNode *idx;
        } idx_access;

        // TT_SLICE_EXPR
        struct {
            TTNode *object;
            TTNode *start;   // may be NULL
            TTNode *end;     // may be NULL
            bool from_array; // true when source is an array (copy semantics)
        } slice_expr;

        // TT_TUPLE_IDX
        struct {
            TTNode *object;
            int32_t elem_idx;
        } tuple_idx;

        // TT_UNARY
        struct {
            TokenKind op;
            TTNode *operand;
        } unary;

        // TT_BINARY
        struct {
            TokenKind op;
            TTNode *left;
            TTNode *right;
        } binary;

        // TT_CALL
        struct {
            TTNode *callee;
            TTNode **args; /* buf */
        } call;

        // TT_TYPE_CONVERSION
        struct {
            TTNode *operand;
            const Type *target_type;
        } type_conversion;

        // TT_IF
        struct {
            TTNode *cond;
            TTNode *then_body;
            TTNode *else_body;
        } if_expr;

        // TT_BLOCK
        struct {
            TTNode **stmts; /* buf */
            TTNode *result;
        } block;

        // TT_LOOP
        struct {
            TTNode *body;
        } loop;

        // TT_DEFER
        struct {
            TTNode *body;
        } defer_stmt;

        // TT_STRUCT_DECL
        struct {
            const char *name;
            const Type *struct_type;
        } struct_decl;

        // TT_STRUCT_LIT
        struct {
            const char **field_names; /* buf */
            TTNode **field_values;    /* buf */
        } struct_lit;

        // TT_STRUCT_FIELD_ACCESS
        struct {
            TTNode *object;
            const char *field;
            bool via_ptr;
        } struct_field_access;

        // TT_METHOD_CALL
        struct {
            TTNode *recv;
            const char *mangled_name;
            TTNode **args; /* buf */
            bool is_ptr_recv;
        } method_call;

        // TT_HEAP_ALLOC
        struct {
            TTNode *operand;
        } heap_alloc;

        // TT_ADDRESS_OF
        struct {
            TTNode *operand;
        } address_of;

        // TT_DEREF
        struct {
            TTNode *operand;
        } deref;

        // TT_ENUM_DECL
        struct {
            const char *name;
            const Type *enum_type;
        } enum_decl;

        // TT_MATCH
        struct {
            TTNode *operand;
            TTNode **arm_conds;    /* buf - pattern cond per arm */
            TTNode **arm_guards;   /* buf - guard expr per arm (NULL if none) */
            TTNode **arm_bodies;   /* buf - body expr per arm */
            TTNode **arm_bindings; /* buf - block of binding stmts (NULL if none) */
        } match_expr;
    };
};

// ── TTNode constructors ───────────────────────────────────────────────

/** Allocate a zero-initialised TTNode from @p arena. */
TTNode *tt_new(Arena *arena, TtNodeKind kind, const Type *type, SourceLoc loc);
/** Allocate a TTSym from @p arena. */
TTSym *tt_sym_new(Arena *arena, TtSymKind kind, Sym *sema_sym, bool is_mut, SourceLoc loc);

// ── TT dump ───────────────────────────────────────────────────────────

/** Recursively pretty-print @p node to stderr at @p indent levels. */
void tt_dump(const TTNode *node, int32_t indent);

// ── TT child visitor ──────────────────────────────────────────────────

/** Callback invoked for each child ptr of a TTNode. */
typedef void (*TtChildVisitor)(void *ctx, TTNode **child_ptr);
/** Call @p visitor for every child ptr in @p node. */
void tt_visit_children(TTNode *node, TtChildVisitor visitor, void *ctx);

#endif // RG_TYPE_TREE_H
