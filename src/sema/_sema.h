#ifndef RG__SEMA_H
#define RG__SEMA_H

#include "sema.h"
#include "types.h"

/**
 * @file _sema.h
 * @brief Internal declarations shared across sema translation units.
 *
 * Not part of the public API -- only included by src/sema/ files.
 */

// ── Struct definitions ─────────────────────────────────────────────────

/** Symbol table entry - one per declared name in a scope. */
struct Symbol {
    const char *name;
    const Type *type;
    bool is_public;
    bool is_function;
    struct Symbol *next; // intrusive linked list within a Scope
};

/**
 * Lexical scope - a linked list of Symbols with a pointer to the
 * enclosing scope.
 */
struct Scope {
    Symbol *symbols;         // head of the symbol chain (may be NULL)
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
    int32_t parameter_count;
    bool is_public;
} FunctionSignature;

struct SemanticAnalyzer {
    Arena *arena;
    Scope *current_scope;
    int32_t error_count;
    TypeAlias *type_aliases;                /* buf */
    FunctionSignature *function_signatures; /* buf */
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
                  bool is_function);
/** Look up @p name in the innermost scope only (for redefinition checks). */
Symbol *scope_lookup_current(const SemanticAnalyzer *analyzer, const char *name);
/** Walk the scope chain outward to find @p name. */
Symbol *scope_lookup(const SemanticAnalyzer *analyzer, const char *name);
/** Return true if any enclosing scope has is_loop set. */
bool in_loop(const SemanticAnalyzer *analyzer);

// ── Type resolution (resolve.c) ────────────────────────────────────────

/** Look up a type alias by name.  Returns the underlying type or NULL. */
const Type *find_type_alias(const SemanticAnalyzer *analyzer, const char *name);
/** Look up a function signature by name. */
FunctionSignature *find_function_signature(const SemanticAnalyzer *analyzer, const char *name);
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

// ── Statement checking (statement.c) ───────────────────────────────────

const Type *check_if(SemanticAnalyzer *analyzer, ASTNode *node);
const Type *check_block(SemanticAnalyzer *analyzer, ASTNode *node);
const Type *check_variable_declaration(SemanticAnalyzer *analyzer, ASTNode *node);
void check_function_body(SemanticAnalyzer *analyzer, ASTNode *function_node);
const Type *check_assign(SemanticAnalyzer *analyzer, ASTNode *node);
const Type *check_compound_assign(SemanticAnalyzer *analyzer, ASTNode *node);

#endif // RG__SEMA_H
