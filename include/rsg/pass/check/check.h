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

#endif // RSG_CHECK_H
