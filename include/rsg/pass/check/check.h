#ifndef RSG_CHECK_H
#define RSG_CHECK_H

#include "repr/ast.h"

/**
 * @file check.h
 * @brief Semantic analysis - scope resolution, type inference, and type checking.
 *
 * Walks the AST produced by the parser, annotates each node with a resolved
 * Type*, and reports errs for undefined names, type mismatches, etc.
 */
typedef struct Sym Sym;
typedef struct Scope Scope;
typedef struct Sema Sema;

/** Create a semantic sema that allocates auxiliary data from @p arena. */
Sema *sema_create(Arena *arena);
/** Destroy the sema (does not free the arena). */
void sema_destroy(Sema *sema);
/**
 * Run two-pass analysis on @p file: (1) register fn sigs,
 * (2) type-check the full AST.  Returns true when no errs were found.
 */
bool sema_check(Sema *sema, ASTNode *file);

#endif // RSG_CHECK_H
