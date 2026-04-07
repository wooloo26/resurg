#ifndef RSG_CHECK_H
#define RSG_CHECK_H

#include "repr/ast.h"

/**
 * @file check.h
 * @brief Check pass — type-check the AST after resolve has populated tables.
 *
 * Walks the AST produced by the parser, annotates each node with a resolved
 * Type*, and reports errs for undefined names, type mismatches, etc.
 */
typedef struct Sema Sema;

/**
 * Check pass: type-check the full AST.  Assumes sema_resolve() has already
 * populated the symbol tables.  Returns true when no errs were found.
 */
bool sema_check(Sema *sema, ASTNode *file);

/** Type-check a fn body (used by mono pass for deferred instantiation). */
void check_fn_body(Sema *sema, ASTNode *fn_node);

/** Type-check a struct/enum method body under the given owner context. */
void check_struct_method_body(Sema *sema, ASTNode *method, const char *struct_name,
                              const Type *struct_type);

#endif // RSG_CHECK_H
