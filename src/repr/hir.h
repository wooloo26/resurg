#ifndef RSG_HIR_H
#define RSG_HIR_H

#include "core/common.h"
#include "core/token.h"
#include "repr/types.h"

/**
 * @file hir.h
 * @brief HIR — fully typed, desugared intermediate representation.
 *
 * Every HIR node carries a resolved type (never NULL), desugared constructs,
 * scope-resolved ids (HirSym with owned name and type), and no
 * syntactic sugar.
 */

typedef struct HirNode HirNode;
typedef struct HirSym HirSym;

// ── HirSym ───────────────────────────────────────────────────────────

/** Sym kind in the HIR — authoritative for lower and codegen. */
typedef enum {
    HIR_SYM_VAR,
    HIR_SYM_PARAM,
    HIR_SYM_FN,
    HIR_SYM_TYPE,
    HIR_SYM_MODULE,
} HirSymKind;

/**
 * HIR sym — owns name and type directly.
 * HirSym.kind is authoritative in HIR/codegen.
 */
struct HirSym {
    HirSymKind kind;
    const char *name;
    const Type *type;
    bool is_mut;
    bool is_ptr_recv; // for method syms: recv is *T
    const char *mangled_name;
    SrcLoc loc;
};

/** Return the name of a HirSym. */
const char *hir_sym_name(const HirSym *sym);
/** Return the type of a HirSym. */
const Type *hir_sym_type(const HirSym *sym);

// ── HirNodeKind ─────────────────────────────────────────────────────────

/** Discriminator for HirNode tagged union. */
typedef enum {
    // Top-level
    HIR_FILE,
    HIR_MODULE,
    HIR_TYPE_ALIAS,

    // Declarations
    HIR_FN_DECL,
    HIR_PARAM,

    // Statements
    HIR_VAR_DECL,
    HIR_RETURN,
    HIR_ASSIGN,
    HIR_BREAK,
    HIR_CONTINUE,
    HIR_DEFER,

    // Lits
    HIR_BOOL_LIT,
    HIR_INT_LIT,
    HIR_FLOAT_LIT,
    HIR_CHAR_LIT,
    HIR_STR_LIT,
    HIR_UNIT_LIT,
    HIR_ARRAY_LIT,
    HIR_SLICE_LIT,
    HIR_TUPLE_LIT,

    // References
    HIR_VAR_REF,
    HIR_MODULE_ACCESS,
    HIR_IDX,
    HIR_SLICE_EXPR,
    HIR_TUPLE_IDX,

    // Operations
    HIR_UNARY,
    HIR_BINARY,
    HIR_CALL,
    HIR_TYPE_CONVERSION,

    // Control flow
    HIR_IF,
    HIR_BLOCK,
    HIR_LOOP,

    // Struct-related
    HIR_STRUCT_DECL,
    HIR_STRUCT_LIT,
    HIR_STRUCT_FIELD_ACCESS,
    HIR_METHOD_CALL,

    // Pointer-related
    HIR_HEAP_ALLOC,
    HIR_ADDRESS_OF,
    HIR_DEREF,

    // Enum-related
    HIR_ENUM_DECL,
    HIR_MATCH,
} HirNodeKind;

// ── HirNode ─────────────────────────────────────────────────────────────

/**
 * Typed Tree node — a tagged union covering every desugared construct.
 * All nodes embed kind, type, and loc as a common header.
 * The type field is never NULL; use TYPE_UNIT for non-expr nodes
 * and TYPE_ERR for poison.
 */
struct HirNode {
    HirNodeKind kind;
    const Type *type;
    SrcLoc loc;

    union {
        // HIR_FILE
        struct {
            HirNode **decls;             /* buf */
            const Type **compound_types; /* buf */
        } file;

        // HIR_MODULE
        struct {
            const char *name;
        } module;

        // HIR_TYPE_ALIAS
        struct {
            const char *name;
            bool is_pub;
            const Type *underlying;
        } type_alias;

        // HIR_FN_DECL
        struct {
            const char *name;
            bool is_pub;
            HirSym *sym;
            HirNode **params; /* buf */
            const Type *return_type;
            HirNode *body;
        } fn_decl;

        // HIR_PARAM
        struct {
            HirSym *sym;
            const char *name;
            const Type *param_type;
            bool is_recv;
            bool is_mut_recv;
            bool is_ptr_recv;
        } param;

        // HIR_VAR_DECL
        struct {
            HirSym *sym;
            const char *name;
            const Type *var_type;
            HirNode *init;
            bool is_mut;
        } var_decl;

        // HIR_RETURN
        struct {
            HirNode *value;
        } return_stmt;

        // HIR_ASSIGN
        struct {
            HirNode *target;
            HirNode *value;
        } assign;

        // HIR_BREAK
        struct {
            HirNode *value;
        } break_stmt;

        // HIR_BOOL_LIT
        struct {
            bool value;
        } bool_lit;

        // HIR_INT_LIT
        struct {
            uint64_t value;
            TypeKind int_kind;
        } int_lit;

        // HIR_FLOAT_LIT
        struct {
            double value;
            TypeKind float_kind;
        } float_lit;

        // HIR_CHAR_LIT
        struct {
            uint32_t value;
        } char_lit;

        // HIR_STR_LIT
        struct {
            const char *value;
        } str_lit;

        // HIR_ARRAY_LIT
        struct {
            HirNode **elems; /* buf */
        } array_lit;

        // HIR_SLICE_LIT
        struct {
            HirNode **elems; /* buf */
        } slice_lit;

        // HIR_TUPLE_LIT
        struct {
            HirNode **elems; /* buf */
        } tuple_lit;

        // HIR_VAR_REF
        struct {
            HirSym *sym;
        } var_ref;

        // HIR_MODULE_ACCESS
        struct {
            HirNode *object;
            const char *member;
        } module_access;

        // HIR_IDX
        struct {
            HirNode *object;
            HirNode *idx;
        } idx_access;

        // HIR_SLICE_EXPR
        struct {
            HirNode *object;
            HirNode *start;  // may be NULL
            HirNode *end;    // may be NULL
            bool from_array; // true when src is an array (copy semantics)
        } slice_expr;

        // HIR_TUPLE_IDX
        struct {
            HirNode *object;
            int32_t elem_idx;
        } tuple_idx;

        // HIR_UNARY
        struct {
            TokenKind op;
            HirNode *operand;
        } unary;

        // HIR_BINARY
        struct {
            TokenKind op;
            HirNode *left;
            HirNode *right;
        } binary;

        // HIR_CALL
        struct {
            HirNode *callee;
            HirNode **args; /* buf */
        } call;

        // HIR_TYPE_CONVERSION
        struct {
            HirNode *operand;
            const Type *target_type;
        } type_conversion;

        // HIR_IF
        struct {
            HirNode *cond;
            HirNode *then_body;
            HirNode *else_body;
        } if_expr;

        // HIR_BLOCK
        struct {
            HirNode **stmts; /* buf */
            HirNode *result;
        } block;

        // HIR_LOOP
        struct {
            HirNode *body;
        } loop;

        // HIR_DEFER
        struct {
            HirNode *body;
        } defer_stmt;

        // HIR_STRUCT_DECL
        struct {
            const char *name;
            const Type *struct_type;
        } struct_decl;

        // HIR_STRUCT_LIT
        struct {
            const char **field_names; /* buf */
            HirNode **field_values;   /* buf */
        } struct_lit;

        // HIR_STRUCT_FIELD_ACCESS
        struct {
            HirNode *object;
            const char *field;
            bool via_ptr;
        } struct_field_access;

        // HIR_METHOD_CALL
        struct {
            HirNode *recv;
            const char *mangled_name;
            HirNode **args; /* buf */
            bool is_ptr_recv;
        } method_call;

        // HIR_HEAP_ALLOC
        struct {
            HirNode *operand;
        } heap_alloc;

        // HIR_ADDRESS_OF
        struct {
            HirNode *operand;
        } address_of;

        // HIR_DEREF
        struct {
            HirNode *operand;
        } deref;

        // HIR_ENUM_DECL
        struct {
            const char *name;
            const Type *enum_type;
        } enum_decl;

        // HIR_MATCH
        struct {
            HirNode *operand;
            HirNode **arm_conds;    /* buf - pattern cond per arm */
            HirNode **arm_guards;   /* buf - guard expr per arm (NULL if none) */
            HirNode **arm_bodies;   /* buf - body expr per arm */
            HirNode **arm_bindings; /* buf - block of binding stmts (NULL if none) */
        } match_expr;
    };
};

// ── HirNode constructors ───────────────────────────────────────────────

/** Allocate a zero-initialised HirNode from @p arena. */
HirNode *hir_new(Arena *arena, HirNodeKind kind, const Type *type, SrcLoc loc);

/** Grouped params for creating a HirSym. */
typedef struct {
    HirSymKind kind;
    const char *name;
    const Type *type;
    bool is_mut;
    SrcLoc loc;
} HirSymSpec;

/** Allocate a HirSym from @p arena using grouped @p spec. */
HirSym *hir_sym_new(Arena *arena, const HirSymSpec *spec);

// ── HIR dump ──────────────────────────────────────────────────────────

/** Recursively pretty-print @p node to stderr at @p indent levels. */
void hir_dump(const HirNode *node, int32_t indent);

// ── HIR child visitor ─────────────────────────────────────────────────

/** Callback invoked for each child ptr of a HirNode. */
typedef void (*HirChildVisitor)(void *ctx, HirNode **child_ptr);
/** Call @p visitor for every child ptr in @p node. */
void hir_visit_children(HirNode *node, HirChildVisitor visitor, void *ctx);

#endif // RSG_HIR_H
