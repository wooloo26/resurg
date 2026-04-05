#ifndef RG__SEMA_H
#define RG__SEMA_H

#include "rsg/sema.h"
#include "types/types.h"

/**
 * @file _sema.h
 * @brief Internal declarations shared across sema translation units.
 *
 * Not part of the public API -- only included by lib/middle/sema/ files.
 */

// ── Struct definitions ─────────────────────────────────────────────────

/** Discriminator for Symbol entries in the scope table. */
typedef enum {
    SYM_VAR,
    SYM_PARAM,
    SYM_FUNCTION,
    SYM_TYPE,
    SYM_MODULE,
} SymbolKind;

/** Symbol table entry - one per declared name in a scope. */
struct Symbol {
    const char *name;
    const Type *type;
    SymbolKind kind;
    bool is_public;
    bool is_immut;
    const ASTNode *declaration;
    Symbol *owner;
};

/**
 * Lexical scope - hash table of Symbols with a pointer to the
 * enclosing scope.
 */
struct Scope {
    HashTable table;         // name → Symbol* (arena-backed)
    struct Scope *parent;    // enclosing scope (NULL for global)
    bool is_loop;            // true inside loop/for bodies (enables break/continue)
    const char *module_name; // propagated from the module declaration
};

/** Type alias entry - registered during the first pass. */
typedef struct TypeAlias {
    const char *name;
    const Type *underlying;
} TypeAlias;

/**
 * Stored function signature - registered in pass 1 so that forward calls
 * can be type-checked in pass 2.
 */
typedef struct FunctionSignature {
    const char *name;
    const Type *return_type;
    const Type **parameter_types; /* buf */
    const char **parameter_names; /* buf */
    int32_t parameter_count;
    bool is_public;
} FunctionSignature;

/** A field definition with its default value expression. */
typedef struct {
    const char *name;
    const Type *type;
    ASTNode *default_value; // may be NULL (field is required)
} StructFieldInfo;

/** A method definition inside a struct. */
typedef struct {
    const char *name;
    bool is_mut_receiver;
    const char *receiver_name;
    ASTNode *declaration;
} StructMethodInfo;

/** Struct definition — registered during the first pass. */
typedef struct {
    const char *name;
    StructFieldInfo *fields;   /* buf */
    StructMethodInfo *methods; /* buf */
    const char **embedded;     /* buf */
    const Type *type;          // resolved TYPE_STRUCT
} StructDefinition;

struct SemanticAnalyzer {
    Arena *arena;
    Scope *current_scope;
    int32_t error_count;
    HashTable type_alias_table; // name → const Type*
    HashTable function_table;   // name → FunctionSignature*
    HashTable struct_table;     // name → StructDefinition*
};

/** Report a semantic error and bump the analyzer's error counter. */
#define SEMA_ERROR(analyzer, location, ...)                                                        \
    do {                                                                                           \
        rsg_error(location, __VA_ARGS__);                                                          \
        (analyzer)->error_count++;                                                                 \
    } while (0)

// ── Scope manipulation (scope.c) ───────────────────────────────────────

/** Push a new child scope.  If @p is_loop is true, break/continue are legal inside it. */
Scope *scope_push(SemanticAnalyzer *analyzer, bool is_loop);
/** Pop the innermost scope. */
void scope_pop(SemanticAnalyzer *analyzer);
/** Define @p name in the current scope. */
void scope_define(SemanticAnalyzer *analyzer, const char *name, const Type *type, bool is_public,
                  SymbolKind kind);
/** Look up @p name in the innermost scope only (for redefinition checks). */
Symbol *scope_lookup_current(const SemanticAnalyzer *analyzer, const char *name);
/** Walk the scope chain outward to find @p name. */
Symbol *scope_lookup(const SemanticAnalyzer *analyzer, const char *name);
/** Return true if any enclosing scope has is_loop set. */
bool in_loop(const SemanticAnalyzer *analyzer);

// ── Type resolution (resolve.c) ────────────────────────────────────────

/** Look up a type alias by name.  Returns the underlying type or NULL. */
const Type *sema_lookup_type_alias(const SemanticAnalyzer *analyzer, const char *name);
/** Look up a function signature by name. */
FunctionSignature *sema_lookup_function(const SemanticAnalyzer *analyzer, const char *name);
/** Look up a struct definition by name. */
StructDefinition *sema_lookup_struct(const SemanticAnalyzer *analyzer, const char *name);
/**
 * Map a syntactic ASTType to a resolved Type*.  Returns NULL for inferred
 * types; emits an error and returns TYPE_ERROR for unknown names.
 */
const Type *resolve_ast_type(SemanticAnalyzer *analyzer, const ASTType *ast_type);
/** Map a literal kind to its corresponding type. */
const Type *literal_kind_to_type(LiteralKind kind);
/** Return the LiteralKind for a given TypeKind. */
LiteralKind type_to_literal_kind(TypeKind kind);
/**
 * Promote a literal node to match @p target's numeric type.
 * Returns @p target on success, NULL if no promotion applies.
 */
const Type *promote_literal(ASTNode *literal, const Type *target);

// ── Node dispatch (statement.c) ────────────────────────────────────────

/** Recursive AST walk - type-checks each node and returns its resolved type. */
const Type *check_node(SemanticAnalyzer *analyzer, ASTNode *node);

// ── Expression checking (expression.c) ─────────────────────────────────

const Type *check_literal(SemanticAnalyzer *analyzer, ASTNode *node);
const Type *check_identifier(SemanticAnalyzer *analyzer, ASTNode *node);
const Type *check_unary(SemanticAnalyzer *analyzer, ASTNode *node);
const Type *check_binary(SemanticAnalyzer *analyzer, ASTNode *node);
const Type *check_call(SemanticAnalyzer *analyzer, ASTNode *node);
const Type *check_member(SemanticAnalyzer *analyzer, ASTNode *node);
const Type *check_index(SemanticAnalyzer *analyzer, ASTNode *node);
const Type *check_type_conversion(SemanticAnalyzer *analyzer, ASTNode *node);
const Type *check_string_interpolation(SemanticAnalyzer *analyzer, ASTNode *node);
const Type *check_array_literal(SemanticAnalyzer *analyzer, ASTNode *node);
const Type *check_tuple_literal(SemanticAnalyzer *analyzer, ASTNode *node);
const Type *check_struct_literal(SemanticAnalyzer *analyzer, ASTNode *node);
const Type *check_address_of(SemanticAnalyzer *analyzer, ASTNode *node);
const Type *check_deref(SemanticAnalyzer *analyzer, ASTNode *node);

// ── Statement checking (statement.c) ───────────────────────────────────

const Type *check_if(SemanticAnalyzer *analyzer, ASTNode *node);
const Type *check_block(SemanticAnalyzer *analyzer, ASTNode *node);
const Type *check_variable_declaration(SemanticAnalyzer *analyzer, ASTNode *node);
void check_function_body(SemanticAnalyzer *analyzer, ASTNode *function_node);
const Type *check_assign(SemanticAnalyzer *analyzer, ASTNode *node);
const Type *check_compound_assign(SemanticAnalyzer *analyzer, ASTNode *node);

#endif // RG__SEMA_H
