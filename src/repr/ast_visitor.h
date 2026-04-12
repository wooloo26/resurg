#ifndef RSG_AST_VISITOR_H
#define RSG_AST_VISITOR_H

#include "repr/ast.h"

/**
 * @file ast_visitor.h
 * @brief AST visitor framework — walk AST trees with per-category callbacks.
 *
 * Mirrors the HirVisitor pattern.  Implement an ASTVisitor by setting
 * only the callbacks you need (NULL callbacks are skipped).
 * The generic ast_walk() dispatcher handles recursive tree traversal.
 *
 * @code
 *     ASTVisitor visitor = {
 *         .ctx = my_ctx,
 *         .visit_fn_decl = my_fn_handler,
 *         .visit_expr    = my_expr_handler,
 *     };
 *     ast_walk(root, &visitor);
 * @endcode
 */

typedef struct ASTVisitor ASTVisitor;

/**
 * Typed AST visitor — callbacks for each major node category.
 *
 * Callbacks are organised by category; the callback receives the concrete
 * node and can switch on @c node->kind for finer dispatch.
 *
 * Set @c pre_visit to receive every node before its children are walked
 * (return false to skip subtree).  Set @c post_visit to receive every
 * node after its children have been walked.
 */
struct ASTVisitor {
    void *ctx; // opaque user context

    // ── Per-category callbacks (called during walk) ────────────────

    /** Called for NODE_FN_DECL. */
    void (*visit_fn_decl)(ASTVisitor *v, const ASTNode *node);
    /** Called for NODE_STRUCT_DECL. */
    void (*visit_struct_decl)(ASTVisitor *v, const ASTNode *node);
    /** Called for NODE_ENUM_DECL. */
    void (*visit_enum_decl)(ASTVisitor *v, const ASTNode *node);
    /** Called for NODE_VAR_DECL. */
    void (*visit_var_decl)(ASTVisitor *v, const ASTNode *node);
    /** Called for NODE_TYPE_ALIAS. */
    void (*visit_type_alias)(ASTVisitor *v, const ASTNode *node);

    /** Called for expression nodes (lits, ids, ops, calls, etc.). */
    void (*visit_expr)(ASTVisitor *v, const ASTNode *node);
    /** Called for statement nodes (return, break, continue, defer, expr_stmt). */
    void (*visit_stmt)(ASTVisitor *v, const ASTNode *node);

    // ── Generic pre/post hooks ─────────────────────────────────────

    /**
     * Called before visiting children.  Return false to skip the subtree.
     * NULL is treated as "always visit".
     */
    bool (*pre_visit)(ASTVisitor *v, const ASTNode *node);
    /** Called after all children have been visited. */
    void (*post_visit)(ASTVisitor *v, const ASTNode *node);
};

/**
 * Walk the AST tree rooted at @p root, dispatching to @p visitor callbacks.
 *
 * Traversal is depth-first, pre-order.  The walk respects the pre_visit
 * gate: if pre_visit returns false the subtree is skipped.
 */
void ast_walk(const ASTNode *root, ASTVisitor *visitor);

#endif // RSG_AST_VISITOR_H
