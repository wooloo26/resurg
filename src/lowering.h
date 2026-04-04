#ifndef RG_LOWERING_H
#define RG_LOWERING_H

#include "tt.h"

typedef struct ASTNode ASTNode;

/**
 * @file lowering.h
 * @brief AST → Typed Tree lowering pass.
 *
 * Desugars compound assignment, for-loops, string interpolation, and
 * tuple member access.  Binds every identifier to a TtSymbol.
 * The returned TT is allocated from its own arena and can be freed
 * independently of the Sema data.
 */
typedef struct Lowering Lowering;

/** Create a lowering context that allocates TT nodes from @p tt_arena. */
Lowering *lowering_create(Arena *tt_arena, Arena *sema_arena);
/** Destroy the lowering context (does not destroy arenas). */
void lowering_destroy(Lowering *lowering);
/**
 * Lower a type-checked AST file node into a Typed Tree.
 * Returns the root TT_FILE node.
 */
TtNode *lowering_lower(Lowering *lowering, const ASTNode *file);

#endif // RG_LOWERING_H
