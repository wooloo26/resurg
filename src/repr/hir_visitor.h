#ifndef RSG_HIR_VISITOR_H
#define RSG_HIR_VISITOR_H

#include "repr/hir.h"

/**
 * @file hir_visitor.h
 * @brief HIR visitor framework — walk HIR trees with per-kind callbacks.
 *
 * New backends and analysis passes implement a HirVisitor by setting
 * only the callbacks they care about (NULL callbacks are skipped).
 * The generic hir_walk() dispatcher handles recursive tree traversal.
 *
 * @code
 *     HirVisitor visitor = {
 *         .ctx = my_ctx,
 *         .visit_fn_decl  = my_fn_handler,
 *         .visit_var_decl = my_var_handler,
 *         .visit_expr     = my_expr_handler,
 *     };
 *     hir_walk(root, &visitor);
 * @endcode
 */

typedef struct HirVisitor HirVisitor;

/**
 * Typed HIR visitor — callbacks for each major node category.
 *
 * For maximum flexibility, callbacks are organised by category rather
 * than individual HirNodeKind.  The callback receives the concrete node
 * and can switch on @c node->kind for finer dispatch.
 *
 * Set @c pre_visit to receive every node before its children are walked
 * (return false to skip subtree).  Set @c post_visit to receive every
 * node after its children have been walked.
 */
struct HirVisitor {
    void *ctx; // opaque user context

    // ── Per-category callbacks (called during walk) ────────────────

    /** Called for HIR_FN_DECL nodes. */
    void (*visit_fn_decl)(HirVisitor *v, const HirNode *node);
    /** Called for HIR_STRUCT_DECL nodes. */
    void (*visit_struct_decl)(HirVisitor *v, const HirNode *node);
    /** Called for HIR_ENUM_DECL nodes. */
    void (*visit_enum_decl)(HirVisitor *v, const HirNode *node);
    /** Called for HIR_VAR_DECL nodes. */
    void (*visit_var_decl)(HirVisitor *v, const HirNode *node);
    /** Called for HIR_TYPE_ALIAS nodes. */
    void (*visit_type_alias)(HirVisitor *v, const HirNode *node);

    /** Called for all expression nodes (lits, refs, ops, control flow). */
    void (*visit_expr)(HirVisitor *v, const HirNode *node);
    /** Called for all statement nodes (return, assign, break, continue, defer). */
    void (*visit_stmt)(HirVisitor *v, const HirNode *node);

    // ── Generic pre/post hooks ─────────────────────────────────────

    /**
     * Called before visiting children.  Return false to skip the subtree.
     * NULL is treated as "always visit".
     */
    bool (*pre_visit)(HirVisitor *v, const HirNode *node);
    /** Called after all children have been visited. */
    void (*post_visit)(HirVisitor *v, const HirNode *node);
};

/**
 * Walk the HIR tree rooted at @p root, dispatching to @p visitor callbacks.
 *
 * Traversal is depth-first, pre-order.  The walk respects the pre_visit
 * gate: if pre_visit returns false, the subtree is skipped.
 */
void hir_walk(const HirNode *root, HirVisitor *visitor);

#endif // RSG_HIR_VISITOR_H
