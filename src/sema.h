#ifndef RG_SEMA_H
#define RG_SEMA_H

#include "ast.h"

/**
 * @file sema.h
 * @brief Semantic analysis - scope resolution, type inference, and type checking.
 *
 * Walks the AST produced by the parser, annotates each node with a resolved
 * Type*, and reports errors for undefined names, type mismatches, etc.
 */
typedef struct Symbol Symbol;
typedef struct Scope Scope;
typedef struct SemanticAnalyzer SemanticAnalyzer;

/** Create a semantic analyzer that allocates auxiliary data from @p arena. */
SemanticAnalyzer *semantic_analyzer_create(Arena *arena);
/** Destroy the analyzer (does not free the arena). */
void semantic_analyzer_destroy(SemanticAnalyzer *analyzer);
/**
 * Run two-pass analysis on @p file: (1) register function signatures,
 * (2) type-check the full AST.  Returns true when no errors were found.
 */
bool semantic_analyzer_check(SemanticAnalyzer *analyzer, ASTNode *file);

#endif // RG_SEMA_H
