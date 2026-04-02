#ifndef RG_SEMA_H
#define RG_SEMA_H

#include "ast.h"
#include "types.h"

// ------------------------------------------------------------------------
// Semantic analysis — type inference, type checking, scope resolution.
// Walks the AST and annotates / validates before codegen.
// ------------------------------------------------------------------------
typedef struct Symbol Symbol;
typedef struct Scope Scope;
typedef struct Sema Sema;

// Create a semantic analyzer.
Sema *sema_create(Arena *arena);
// Destroy the semantic analyzer.
void sema_destroy(Sema *s);
// Type-check the AST; returns true if no errors.
bool sema_check(Sema *s, ASTNode *file);

#endif // RG_SEMA_H
