#ifndef RSG__SEMA_H
#define RSG__SEMA_H

#include "core/diag.h"
#include "core/intrinsic.h"
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
    bool is_declare;         // true for decl fns
    bool has_variadic;       // true when last param is variadic (..T)
    IntrinsicKind intrinsic; // INTRINSIC_NONE for user fns; set once during resolve
};

/** Base for all generic templates (fn, struct, enum). */
typedef struct GenericDef {
    const char *name;
    ASTNode *decl;             // original AST node
    ASTTypeParam *type_params; /* buf */
    int32_t type_param_count;
} GenericDef;

/** Generic fn/struct/enum share the same layout — use GenericDef directly. */
typedef GenericDef GenericFnDef;
typedef GenericDef GenericStructDef;
typedef GenericDef GenericEnumDef;

/** Tracks a pending generic instantiation for deferred body checking. */
typedef struct GenericInst {
    GenericDef *generic;      // source template
    const char *mangled_name; // monomorphized fn name
    const Type **type_args;   /* buf - resolved concrete types */
    ASTNode *file_node;       // file AST to append cloned fn to
} GenericInst;

/** Generic type alias — adds unresolved alias type to the base. */
typedef struct GenericTypeAlias {
    GenericDef base;
    ASTType alias_type; // unresolved template type
} GenericTypeAlias;

/** Generic ext template — adds target type name to the base. */
typedef struct GenericExtDef {
    GenericDef base;
    const char *target_name; // target type name (e.g. "Pair")
} GenericExtDef;

/** A field def with its default value expr. */
typedef struct StructFieldInfo {
    const char *name;
    const Type *type;
    ASTNode *default_value; // may be NULL (field is required)
    bool is_pub;
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
    bool is_tuple_struct;      /* true for `struct Name(T, ...)` */
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
    ASTAssocType *assoc_types; /* buf */
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

/** Callback to load module decls from a file path. Returns NULL on failure. */
typedef ASTNode **(*ModuleLoader)(void *ctx, Arena *arena, const char *mod_path);

/** Callback to type-check a single fn body (used by mono to avoid check dependency). */
typedef void (*FnBodyChecker)(struct Sema *sema, ASTNode *fn_node);

/** Closure context — saved/restored as a unit when entering nested closures. */
typedef struct {
    Scope *scope;               // scope of the enclosing Fn/FnMut closure (NULL if none)
    FnTypeKind fn_kind;         // fn kind of the enclosing closure (FN_PLAIN when not in closure)
    bool has_capture;           // true when any variable outside closure scope is referenced
    bool captures_mutated;      // true when a captured variable is mutated (FnMut inference)
    const char **capture_names; /* buf - collected captured variable names (deduplicated) */
} ClosureCtx;

/** Grouped hash tables for generic definitions and active type params. */
typedef struct GenericTables {
    HashTable fn;          // name → GenericFnDef*
    HashTable structs;     // name → GenericStructDef*
    HashTable enums;       // name → GenericEnumDef*
    HashTable type_alias;  // name → GenericTypeAlias*
    HashTable type_params; // name → const Type* (active during generic body check)
} GenericTables;

/**
 * Variant constructor entry — registered during `pub use Enum::*` so
 * that bare variant calls (e.g. `Some(x)`, `None`) resolve generically
 * instead of via hardcoded string comparisons.
 */
typedef struct VariantCtorInfo {
    const char *enum_name;    // e.g. "Option" — key into generics.enums
    const char *variant_name; // e.g. "Some"
    bool has_payload;         // true for tuple variants (Some(T)), false for unit (None)
} VariantCtorInfo;

/** Module-level immutable variable — registered by `pub immut`. */
typedef struct VarInfo {
    const char *name; // qualified name (e.g. "math.PI")
    const Type *type; // resolved type
    ASTNode *init;    // init expression AST (always non-NULL)
    bool is_pub;      // true for pub immut
} VarInfo;

/** Read-write symbol registry — the global declaration tables. */
typedef struct SemaDB {
    HashTable type_alias_table;   // name → const Type*
    HashTable fn_table;           // name → FnSig*
    HashTable struct_table;       // name → StructDef*
    HashTable enum_table;         // name → EnumDef*
    HashTable pact_table;         // name → PactDef*
    HashTable variant_ctor_table; // bare_name → VariantCtorInfo*
    HashTable var_table;          // name → VarInfo*
} SemaDB;

/** Module-loading context — directory paths and loader callback. */
typedef struct ModuleCtx {
    const char *current;    // current module prefix (e.g. "math_helper"); NULL for root file
    const char *search_dir; // directory for resolving filesystem modules
    const char *std_dir;    // fallback directory for std library modules
    ModuleLoader loader;    // injected callback to load module files
    void *loader_ctx;       // opaque context for the module loader
} ModuleCtx;

/** Per-expression type inference context — saved/restored around sub-checks. */
typedef struct TypeInferCtx {
    const Type *expected_type;   // expected type for current expr (bidirectional inference)
    const Type *loop_break_type; // break value type in current loop (NULL if no break-with-value)
    const Type *fn_return_type;  // return type of the enclosing function (for Ok/Err/None)
    const char *self_type_name;  // enclosing type name for Self resolution (NULL if not in method)
} TypeInferCtx;

// ── Sema phase tracking ────────────────────────────────────────────

/** Active semantic pass — used for phase-violation assertions in debug builds. */
typedef enum {
    SEMA_PHASE_RESOLVE, // name resolution + registration
    SEMA_PHASE_CHECK,   // type-checking + inference
    SEMA_PHASE_MONO,    // generic monomorphization
} SemaPhase;

/**
 * @brief Shared semantic base — infrastructure used by all sema phases.
 *
 * Contains the arena, scope chain, diagnostic collector, and the global
 * declaration + generic-template tables.  Extracted from the full Sema so
 * that helpers needing only shared state can accept @c SemaBase* instead
 * of the full context.  SemaBase is always the first member of Sema,
 * so casting between @c SemaBase* and @c Sema* is safe (C17 §6.7.2.1¶15).
 */
typedef struct SemaBase {
    Arena *arena;
    Scope *current_scope;
    int32_t err_count;
    DiagCtx dctx;           // structured diagnostic collector
    SemaDB db;              // global declaration tables
    GenericTables generics; // generic template tables + active type params
    ASTNode *file_node;     // root file node (for appending monomorphized fns)
    SemaPhase phase;        // current active phase
} SemaBase;

/**
 * @brief Full semantic context — SemaBase plus per-phase fields.
 *
 * Fields are grouped by phase ownership.  Phase assertions enforce that
 * pass-specific fields are only accessed by their owning pass in debug
 * builds.
 */
struct Sema {
    SemaBase base; // MUST be first — enables SemaBase*/Sema* safe casting

    // ── Resolve phase ──────────────────────────────────────
    ModuleCtx module; // module-loading context

    // ── Check phase ────────────────────────────────────────
    MethodChecker method_checker; // set by check pass; NULL during resolve
    TypeInferCtx infer;           // per-expression type inference context
    ClosureCtx closure;           // closure capture tracking

    // ── Mono phase ─────────────────────────────────────────
    FnBodyChecker fn_body_checker; // injected callback for mono to type-check cloned fn bodies
    int32_t mono_depth;            // current monomorphization recursion depth

    // ── Cross-phase (check → mono) ─────────────────────────
    GenericInst *pending_insts;       /* buf - deferred generic instantiations */
    GenericExtDef **generic_ext_defs; /* buf - generic ext templates */
    ASTNode **synthetic_decls;        /* buf - monomorphized struct/enum decls to append */
};

/** Assert that the sema is in the expected @p p phase (debug builds only). */
#ifndef NDEBUG
#define SEMA_ASSERT_PHASE(sema, p) assert((sema)->base.phase == (p) && "sema phase violation")
#else
#define SEMA_ASSERT_PHASE(sema, p) ((void)0)
#endif

/** Report a semantic err and bump the sema's err counter. */
#define SEMA_ERR(sema, loc, ...)                                                                   \
    do {                                                                                           \
        diag_at(&(sema)->base.dctx, DIAG_ERR, loc, NULL, __VA_ARGS__);                             \
        (sema)->base.err_count++;                                                                  \
    } while (0)

// ── TypeInferCtx scope guards ──────────────────────────────────────────

/**
 * Save the current value of a TypeInferCtx field and set a new value.
 * Must be paired with SEMA_INFER_RESTORE in the same scope.
 */
#define SEMA_INFER_SCOPE(sema, field, value)                                                       \
    const __typeof__((sema)->infer.field) _saved_infer_##field = (sema)->infer.field;              \
    (sema)->infer.field = (value)

/** Restore a TypeInferCtx field previously saved by SEMA_INFER_SCOPE. */
#define SEMA_INFER_RESTORE(sema, field) (sema)->infer.field = _saved_infer_##field

#endif // RSG__SEMA_H
