#ifndef RG_TT_PASSES_H
#define RG_TT_PASSES_H

#include "types/type_tree.h"

/**
 * @file tt_passes.h
 * @brief TT-to-TT transformation passes.
 *
 * Each pass walks the tree in-place, rewriting nodes.  Passes are
 * composable and order-independent unless noted otherwise.
 */

/**
 * Fold constant binary and unary operations on integer and float
 * literals into new literal nodes.  Division/modulo by zero is left
 * un-folded.
 */
void tt_pass_const_fold(Arena *arena, TtNode *root);

/**
 * Promote escaping TT_ADDRESS_OF nodes to TT_HEAP_ALLOC.
 *
 * An address-of result escapes if it may outlive the scope of the addressed
 * variable — e.g. returned from a function or used as the function body's
 * result expression.  Conservative rule: if escape status cannot be proven
 * non-escaping, the node is treated as escaping.
 */
void tt_pass_escape_analysis(Arena *arena, TtNode *root);

#endif // RG_TT_PASSES_H
