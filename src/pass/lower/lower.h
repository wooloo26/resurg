#ifndef RSG_LOWER_H
#define RSG_LOWER_H

#include "repr/hir.h"

typedef struct ASTNode ASTNode;

/**
 * @file lower.h
 * @brief AST → HIR lowering pass.
 *
 * Desugars compound assignment, for-loops, str interpolation, and
 * tuple member access.  Binds every id to a HirSym.
 * The returned HIR is allocated from its own arena and can be freed
 * independently of the Sema data.
 */
typedef struct Lower Lower;

/** Create a lower ctx that allocates HIR nodes from @p hir_arena. */
Lower *lower_create(Arena *hir_arena);
/** Destroy the lower ctx (does not destroy arenas). */
void lower_destroy(Lower *lower);
/**
 * Lower a type-checked AST file node into HIR.
 * Returns the root HIR_FILE node.
 */
HirNode *lower_lower(Lower *lower, const ASTNode *file);

#endif // RSG_LOWER_H
