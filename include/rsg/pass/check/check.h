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

/**
 * Type-check a single fn body (params + body block).
 * Used by the mono pass to check cloned generic fn instances
 * without depending on check-pass internals.
 */
void sema_check_fn_body(Sema *sema, ASTNode *fn_node);

/**
 * Enable method body checking on @p sema so that generic type
 * instantiation (which may occur during resolve) can type-check
 * cloned method bodies immediately.
 *
 * Must be called before sema_resolve() when the pipeline needs
 * associated-type-bearing generic enums (e.g. Option<T>) to have
 * fully typed method parameters.
 */
void sema_enable_method_checking(Sema *sema);

#endif // RSG_CHECK_H
