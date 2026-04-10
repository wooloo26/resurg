#ifndef RSG__RESOLVE_H
#define RSG__RESOLVE_H

#include "repr/ast.h"
#include "repr/types.h"

/**
 * @file _resolve.h
 * @brief Name resolution — scope management, symbol lookup, and type resolution.
 *
 * Provides the scope chain, symbol table helpers, and AST-to-Type
 * resolution used by the semantic passes (resolve, check, mono).
 */
typedef struct Sema Sema;
typedef struct Sym Sym;
typedef struct Scope Scope;
typedef struct FnSig FnSig;
typedef struct StructDef StructDef;
typedef struct EnumDef EnumDef;
typedef struct PactDef PactDef;
typedef struct StructFieldInfo StructFieldInfo;
typedef struct StructMethodInfo StructMethodInfo;
typedef struct GenericDef GenericDef;
typedef GenericDef GenericFnDef;
typedef GenericDef GenericStructDef;
typedef GenericDef GenericEnumDef;
typedef struct GenericTypeAlias GenericTypeAlias;

// ── Struct defs ─────────────────────────────────────────────────

/** Discriminator for Sym entries in the scope table. */
typedef enum {
    SYM_VAR,
    SYM_PARAM,
    SYM_FN,
    SYM_TYPE,
    SYM_MODULE,
} SymKind;

/** Sym table entry - one per declared name in a scope. */
struct Sym {
    const char *name;
    const Type *type;
    SymKind kind;
    bool is_pub;
    bool is_immut;
    const ASTNode *decl;
    Sym *owner;
};

/**
 * Lexical scope - hash table of Syms with a ptr to the
 * enclosing scope.
 */
struct Scope {
    HashTable table;         // name → Sym* (arena-backed)
    struct Scope *parent;    // enclosing scope (NULL for global)
    bool is_loop;            // true inside loop/for bodies (enables break/continue)
    const char *module_name; // propagated from the module decl
};

// ── Scope manipulation (resolve_scope.c) ───────────────────────────────

/** Grouped params for defining a sym in a scope. */
typedef struct {
    const char *name;
    const Type *type;
    bool is_pub;
    SymKind kind;
} SymDef;

/** Push a new child scope.  If @p is_loop is true, break/continue are legal inside it. */
Scope *scope_push(Sema *sema, bool is_loop);
/** Pop the innermost scope. */
void scope_pop(Sema *sema);
/** Define a sym in the current scope. */
void scope_define(Sema *sema, const SymDef *def);
/** Look up @p name in the innermost scope only (for redef checks). */
Sym *scope_lookup_current(const Sema *sema, const char *name);
/** Walk the scope chain outward to find @p name. */
Sym *scope_lookup(const Sema *sema, const char *name);
/** Return true if any enclosing scope has is_loop set. */
bool in_loop(const Sema *sema);

// ── Type resolution (resolve_type.c) ───────────────────────────────────

/** Look up a type alias by name.  Returns the underlying type or NULL. */
const Type *sema_lookup_type_alias(const Sema *sema, const char *name);
/** Look up a fn sig by name. */
FnSig *sema_lookup_fn(const Sema *sema, const char *name);
/** Look up a struct def by name. */
StructDef *sema_lookup_struct(const Sema *sema, const char *name);
/** Look up an enum def by name. */
EnumDef *sema_lookup_enum(const Sema *sema, const char *name);
/** Look up a pact def by name. */
PactDef *sema_lookup_pact(const Sema *sema, const char *name);
/** Look up a generic fn template by name. */
GenericFnDef *sema_lookup_generic_fn(const Sema *sema, const char *name);
/** Look up a generic struct template by name. */
GenericStructDef *sema_lookup_generic_struct(const Sema *sema, const char *name);
/** Look up a generic enum template by name. */
GenericEnumDef *sema_lookup_generic_enum(const Sema *sema, const char *name);
/** Look up a generic type alias by name. */
GenericTypeAlias *sema_lookup_generic_type_alias(const Sema *sema, const char *name);
/**
 * Map a syntactic ASTType to a resolved Type*.  Returns NULL for inferred
 * types; emits an err and returns TYPE_ERR for unknown names.
 */
const Type *resolve_ast_type(Sema *sema, const ASTType *ast_type);
/** Map a lit kind to its corresponding type. */
const Type *lit_kind_to_type(LitKind kind);
/** Return the LitKind for a given TypeKind. */
LitKind type_to_lit_kind(TypeKind kind);
/**
 * Promote a lit node to match @p target's numeric type.
 * Returns @p target on success, NULL if no promotion applies.
 */
const Type *promote_lit(ASTNode *lit, const Type *target);

// ── Fn registration (resolve_fn.c) ─────────────────────────────────────

/**
 * Create a FnSig from a fn decl, resolving its return type and param types.
 * The caller is responsible for inserting the sig into the fn table.
 */
FnSig *build_fn_sig(Sema *sema, ASTNode *decl, bool is_pub);

/** Register a method sig in the fn table and append to @p methods buf. */
void register_method_sig(Sema *sema, const char *type_name, ASTNode *method,
                         StructMethodInfo **methods);
void register_fn_sig(Sema *sema, ASTNode *decl);

// ── Struct registration (resolve_struct.c) ─────────────────────────────

void register_struct_def(Sema *sema, ASTNode *decl);

// ── Enum/pact registration (resolve_register.c) ───────────────────────

void collect_pact_fields(Sema *sema, const PactDef *pact, StructFieldInfo **fields);
void collect_pact_methods(Sema *sema, const PactDef *pact, StructMethodInfo **methods);
void enforce_pact_assoc_type_bounds(Sema *sema, ASTNode *decl, StructDef *def, const PactDef *pact);
void register_enum_def(Sema *sema, ASTNode *decl);
void register_pact_def(Sema *sema, ASTNode *decl);
void enforce_pact_conformances(Sema *sema, ASTNode *decl, StructDef *def);

// ── Ext registration (resolve_ext.c) ──────────────────────────────────

void register_ext_decl(Sema *sema, ASTNode *decl);
void enforce_ext_pact_conformances(Sema *sema, ASTNode *decl);

// ── Module registration (resolve_module.c) ────────────────────────────

void register_module_decl(Sema *sema, ASTNode *decl);
void register_use_decl(Sema *sema, ASTNode *decl);

/** Load a filesystem module file, lex, and parse it. Returns NULL on failure. */
ASTNode **load_module_decls(Sema *sema, const char *mod_path);

// ── Generic instantiation (sema_generic.c) ─────────────────────────────

/** Grouped params for a generic instantiation request. */
typedef struct {
    ASTType *type_args;
    int32_t type_arg_count;
    SrcLoc loc;
} GenericInstArgs;

/** Check if @p type satisfies the pact bound @p bound_name (recursively). */
bool type_satisfies_bound(Sema *sema, const Type *type, const char *bound_name);
/** Build a mangled name for a generic instantiation: "base__type1_type2". */
const char *build_mangled_name(Sema *sema, const char *base, const Type **type_args, int32_t count);

/**
 * Push type param substitutions into the sema type_param_table, saving
 * any previously bound values.  Caller must pair with sema_pop_type_params.
 */
void sema_push_type_params(Sema *sema, ASTTypeParam *params, const Type **resolved, int32_t count,
                           const Type ***out_saved);

/**
 * Pop type param substitutions, restoring previous values saved by
 * sema_push_type_params.  Frees the @p saved buffer.
 */
void sema_pop_type_params(Sema *sema, ASTTypeParam *params, int32_t count, const Type **saved);

/**
 * Instantiate a generic struct with the given type args.
 * Creates a concrete struct def with a mangled name and registers it.
 * Uses sema->method_checker (if set) to type-check method bodies.
 */
const char *instantiate_generic_struct(Sema *sema, GenericStructDef *gdef,
                                       const GenericInstArgs *args);
/**
 * Instantiate a generic enum with the given type args.
 * Creates a concrete enum def with a mangled name and registers it.
 * Uses sema->method_checker (if set) to type-check method bodies.
 */
const char *instantiate_generic_enum(Sema *sema, GenericEnumDef *gdef, const GenericInstArgs *args);

/**
 * Instantiate generic ext templates for compound types (slices, arrays).
 * Searches generic_ext_defs for templates targeting @p compound_key,
 * registers concrete methods under @p concrete_name, and appends a synthetic
 * ext decl for the lowering pass.
 * @return The FnSig for @p method_name if found after instantiation, else NULL.
 */
FnSig *instantiate_compound_ext(Sema *sema, const char *compound_key, const Type *obj_type,
                                const char *concrete_name, const char *method_name);

#endif // RSG__RESOLVE_H
