#ifndef RSG__SEMA_H
#define RSG__SEMA_H

#include "pass/resolve/_resolve.h"

/**
 * @file _sema.h
 * @brief Shared semantic context — Sema struct and registration types.
 *
 * Included by resolve, check, and mono translation units.
 * Separates the semantic context from the pass-specific dispatch functions.
 */

// ── Registration structs ────────────────────────────────────────

/** Type alias entry - registered during the first pass. */
typedef struct TypeAlias {
    const char *name;
    const Type *underlying;
} TypeAlias;

/**
 * Stored fn sig - registered in pass 1 so that forward calls
 * can be type-checked in pass 2.
 */
struct FnSig {
    const char *name;
    const Type *return_type;
    const Type **param_types; /* buf */
    const char **param_names; /* buf */
    int32_t param_count;
    bool is_pub;
    bool is_ptr_recv;
};

/** Generic fn template — stored when a fn has type params. */
typedef struct GenericFnDef {
    const char *name;
    ASTNode *decl;             // original fn_decl AST
    ASTTypeParam *type_params; /* buf */
    int32_t type_param_count;
} GenericFnDef;

/** Tracks a pending generic instantiation for deferred body checking. */
typedef struct GenericInst {
    GenericFnDef *generic;    // source template
    const char *mangled_name; // monomorphized fn name
    const Type **type_args;   /* buf - resolved concrete types */
    ASTNode *file_node;       // file AST to append cloned fn to
} GenericInst;

/** Generic struct template — stored when a struct has type params. */
typedef struct GenericStructDef {
    const char *name;
    ASTNode *decl;             // original struct_decl AST
    ASTTypeParam *type_params; /* buf */
    int32_t type_param_count;
} GenericStructDef;

/** Generic enum template — stored when an enum has type params. */
typedef struct GenericEnumDef {
    const char *name;
    ASTNode *decl;             // original enum_decl AST
    ASTTypeParam *type_params; /* buf */
    int32_t type_param_count;
} GenericEnumDef;

/** Generic type alias — stored when a type alias has type params. */
typedef struct GenericTypeAlias {
    const char *name;
    ASTType alias_type;        // unresolved template type
    ASTTypeParam *type_params; /* buf */
    int32_t type_param_count;
} GenericTypeAlias;

/** Generic ext template — stored when an ext block has type params. */
typedef struct GenericExtDef {
    const char *target_name;   // target type name (e.g. "Pair")
    ASTNode *decl;             // original ext_decl AST
    ASTTypeParam *type_params; /* buf */
    int32_t type_param_count;
} GenericExtDef;

/** A field def with its default value expr. */
typedef struct StructFieldInfo {
    const char *name;
    const Type *type;
    ASTNode *default_value; // may be NULL (field is required)
} StructFieldInfo;

/** A method def inside a struct. */
typedef struct StructMethodInfo {
    const char *name;
    bool is_mut_recv;
    bool is_ptr_recv;
    const char *recv_name;
    ASTNode *decl;
} StructMethodInfo;

/** Struct def — registered during the first pass. */
struct StructDef {
    const char *name;
    StructFieldInfo *fields;   /* buf */
    StructMethodInfo *methods; /* buf */
    const char **embedded;     /* buf */
    ASTAssocType *assoc_types; /* buf */
    const Type *type;          // resolved TYPE_STRUCT
};

/** Enum def — registered during the first pass. */
struct EnumDef {
    const char *name;
    StructMethodInfo *methods; /* buf */
    const Type *type;          // resolved TYPE_ENUM
};

/** Pact (interface) def — registered during the first pass. */
struct PactDef {
    const char *name;
    StructFieldInfo *fields;   /* buf - required fields */
    StructMethodInfo *methods; /* buf - all methods (required + default) */
    const char **super_pacts;  /* buf - constraint alias refs */
    ASTAssocType *assoc_types; /* buf */
};

/** Callback for type-checking a method body during generic instantiation. */
typedef void (*MethodChecker)(struct Sema *sema, ASTNode *method, const char *owner_name,
                              const Type *owner_type);

/** Closure context — saved/restored as a unit when entering nested closures. */
typedef struct {
    Scope *scope;          // scope of the enclosing Fn/FnMut closure (NULL if none)
    FnTypeKind fn_kind;    // fn kind of the enclosing closure (FN_PLAIN when not in closure)
    bool has_capture;      // true when any variable outside closure scope is referenced
    bool captures_mutated; // true when a captured variable is mutated (FnMut inference)
} ClosureCtx;

struct Sema {
    Arena *arena;
    Scope *current_scope;
    int32_t err_count;
    MethodChecker method_checker; // set by check pass; NULL during resolve
    const Type *loop_break_type;  // break value type in current loop (NULL if no break-with-value)
    const Type *expected_type;    // expected type for current expr (bidirectional inference)
    const Type *fn_return_type;   // return type of the enclosing function (for Ok/Err/None)
    const char *self_type_name;   // enclosing type name for Self resolution (NULL if not in method)
    ClosureCtx closure;           // closure capture tracking (check pass)
    ASTNode *file_node;           // root file node (for appending monomorphized fns)
    HashTable type_alias_table;   // name → const Type*
    HashTable fn_table;           // name → FnSig*
    HashTable struct_table;       // name → StructDef*
    HashTable enum_table;         // name → EnumDef*
    HashTable pact_table;         // name → PactDef*
    HashTable generic_fn_table;   // name → GenericFnDef*
    HashTable generic_struct_table;     // name → GenericStructDef*
    HashTable generic_enum_table;       // name → GenericEnumDef*
    HashTable generic_type_alias_table; // name → GenericTypeAlias*
    HashTable type_param_table;         // name → const Type* (active during generic body check)
    GenericInst *pending_insts;         /* buf - deferred generic instantiations */
    GenericExtDef **generic_ext_defs;   /* buf - generic ext templates */
    ASTNode **synthetic_decls;          /* buf - monomorphized struct/enum decls to append */
};

/** Report a semantic err and bump the sema's err counter. */
#define SEMA_ERR(sema, loc, ...)                                                                   \
    do {                                                                                           \
        rsg_err(loc, __VA_ARGS__);                                                                 \
        (sema)->err_count++;                                                                       \
    } while (0)

#endif // RSG__SEMA_H
