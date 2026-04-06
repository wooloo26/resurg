#ifndef RG_SEMA_H
#define RG_SEMA_H

#include "ast/ast.h"

/**
 * @file sema.h
 * @brief Semantic analysis - scope resolution, type inference, and type checking.
 *
 * Walks the AST produced by the parser, annotates each node with a resolved
 * Type*, and reports errs for undefined names, type mismatches, etc.
 */
typedef struct Sym Sym;
typedef struct Scope Scope;
typedef struct Sema Sema;

/** Create a semantic analyzer that allocates auxiliary data from @p arena. */
Sema *sema_create(Arena *arena);
/** Destroy the analyzer (does not free the arena). */
void sema_destroy(Sema *analyzer);
/**
 * Run two-pass analysis on @p file: (1) register fn signatures,
 * (2) type-check the full AST.  Returns true when no errs were found.
 */
bool sema_check(Sema *analyzer, ASTNode *file);

#endif // RG_SEMA_H
