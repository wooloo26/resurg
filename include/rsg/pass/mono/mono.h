#ifndef RSG_PUB_MONO_H
#define RSG_PUB_MONO_H

#include "repr/ast.h"

/**
 * @file mono.h
 * @brief Mono pass — generic monomorphization.
 *
 * Processes pending generic fn instantiations queued during the check
 * pass: clones template bodies, type-checks them with concrete type
 * substitutions, and appends the specialized decls to the file AST.
 */
typedef struct Sema Sema;

/**
 * Mono pass: process all pending generic instantiations and pop the
 * global scope.  Returns true when no errs were found.
 */
bool sema_mono(Sema *sema, ASTNode *file);

#endif // RSG_PUB_MONO_H
