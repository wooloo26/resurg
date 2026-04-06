#ifndef RG_LOWERING_H
#define RG_LOWERING_H

#include "types/tt.h"

typedef struct ASTNode ASTNode;

/**
 * @file lowering.h
 * @brief AST → Typed Tree lowering pass.
 *
 * Desugars compound assignment, for-loops, str interpolation, and
 * tuple member access.  Binds every id to a TTSym.
 * The returned TT is allocated from its own arena and can be freed
 * independently of the Sema data.
 */
typedef struct Lowering Lowering;

/** Create a lowering ctx that allocates TT nodes from @p tt_arena. */
Lowering *lowering_create(Arena *tt_arena);
/** Destroy the lowering ctx (does not destroy arenas). */
void lowering_destroy(Lowering *lowering);
/**
 * Lower a type-checked AST file node into a Typed Tree.
 * Returns the root TT_FILE node.
 */
TTNode *lowering_lower(Lowering *lowering, const ASTNode *file);

#endif // RG_LOWERING_H
