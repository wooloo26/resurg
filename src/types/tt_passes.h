#ifndef RG_TT_PASSES_H
#define RG_TT_PASSES_H

#include "types/tt.h"

/**
 * @file tt_passes.h
 * @brief TT-to-TT transfmtion passes.
 *
 * Each pass walks the tree in-place, rewriting nodes.  Passes are
 * composable and order-independent unless noted otherwise.
 */

/**
 * Fold constant binary and unary operations on integer and float
 * lits into new lit nodes.  Division/modulo by zero is left
 * un-folded.
 */
void tt_pass_const_fold(Arena *arena, TTNode *root);

/**
 * Promote escaping TT_ADDRESS_OF nodes to TT_HEAP_ALLOC.
 *
 * An address-of result escapes if it may outlive the scope of the addressed
 * var — e.g. returned from a fn or used as the fn body's
 * result expr.  Conservative rule: if escape status cannot be proven
 * non-escaping, the node is treated as escaping.
 */
void tt_pass_escape_analysis(Arena *arena, TTNode *root);

#endif // RG_TT_PASSES_H
