#ifndef RSG_HIR_PASSES_H
#define RSG_HIR_PASSES_H

#include "repr/hir.h"

/**
 * @file hir_passes.h
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
void hir_pass_const_fold(Arena *arena, HirNode *root);

/**
 * Promote escaping HIR_ADDRESS_OF nodes to HIR_HEAP_ALLOC.
 *
 * An address-of result escapes if it may outlive the scope of the addressed
 * var — e.g. returned from a fn or used as the fn body's
 * result expr.  Conservative rule: if escape status cannot be proven
 * non-escaping, the node is treated as escaping.
 */
void hir_pass_escape_analysis(Arena *arena, HirNode *root);

#endif // RSG_HIR_PASSES_H
