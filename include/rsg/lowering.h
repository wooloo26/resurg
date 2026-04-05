#ifndef RG_PUB_LOWERING_H
#define RG_PUB_LOWERING_H

#include "types/type_tree.h"

typedef struct ASTNode ASTNode;

/**
 * @file lowering.h
 * @brief Public forwarding header for the AST → Typed Tree lowering pass.
 */
typedef struct Lowering Lowering;

/** Create a lowering context that allocates TT nodes from @p tt_arena. */
Lowering *lowering_create(Arena *tt_arena);
/** Destroy the lowering context (does not destroy arenas). */
void lowering_destroy(Lowering *lowering);
/**
 * Lower a type-checked AST file node into a Typed Tree.
 * Returns the root TT_FILE node.
 */
TtNode *lowering_lower(Lowering *lowering, const ASTNode *file);

#endif // RG_PUB_LOWERING_H
