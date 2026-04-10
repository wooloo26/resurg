#include "repr/hir_visitor.h"

/**
 * @file hir_visitor.c
 * @brief HIR visitor framework — generic tree walker with per-kind dispatch.
 */

// ── Node classification helpers ────────────────────────────────────────

/** Return true if @p kind is an expression node. */
static bool is_expr_kind(HirNodeKind kind) {
    switch (kind) {
    case HIR_BOOL_LIT:
    case HIR_INT_LIT:
    case HIR_FLOAT_LIT:
    case HIR_CHAR_LIT:
    case HIR_STR_LIT:
    case HIR_UNIT_LIT:
    case HIR_ARRAY_LIT:
    case HIR_SLICE_LIT:
    case HIR_TUPLE_LIT:
    case HIR_VAR_REF:
    case HIR_MODULE_ACCESS:
    case HIR_IDX:
    case HIR_SLICE_EXPR:
    case HIR_TUPLE_IDX:
    case HIR_UNARY:
    case HIR_BINARY:
    case HIR_CALL:
    case HIR_TYPE_CONVERSION:
    case HIR_IF:
    case HIR_BLOCK:
    case HIR_LOOP:
    case HIR_STRUCT_LIT:
    case HIR_STRUCT_FIELD_ACCESS:
    case HIR_METHOD_CALL:
    case HIR_HEAP_ALLOC:
    case HIR_ADDRESS_OF:
    case HIR_DEREF:
    case HIR_MATCH:
    case HIR_CLOSURE:
        return true;
    default:
        return false;
    }
}

/** Return true if @p kind is a statement node. */
static bool is_stmt_kind(HirNodeKind kind) {
    switch (kind) {
    case HIR_RETURN:
    case HIR_ASSIGN:
    case HIR_BREAK:
    case HIR_CONTINUE:
    case HIR_DEFER:
        return true;
    default:
        return false;
    }
}

// ── Recursive walk ─────────────────────────────────────────────────────

/** Core walk function. */
static void walk_node(const HirNode *node, HirVisitor *visitor);

/** Adapter: forwards hir_visit_children callbacks into walk_node. */
static void walk_adapter(void *ctx, HirNode **child_ptr) {
    walk_node(*child_ptr, (HirVisitor *)ctx);
}

static void walk_node(const HirNode *node, HirVisitor *visitor) {
    if (node == NULL) {
        return;
    }

    // Pre-visit gate.
    if (visitor->pre_visit != NULL) {
        if (!visitor->pre_visit(visitor, node)) {
            return; // skip subtree
        }
    }

    // Per-category dispatch.
    switch (node->kind) {
    case HIR_FN_DECL:
        if (visitor->visit_fn_decl != NULL) {
            visitor->visit_fn_decl(visitor, node);
        }
        break;
    case HIR_STRUCT_DECL:
        if (visitor->visit_struct_decl != NULL) {
            visitor->visit_struct_decl(visitor, node);
        }
        break;
    case HIR_ENUM_DECL:
        if (visitor->visit_enum_decl != NULL) {
            visitor->visit_enum_decl(visitor, node);
        }
        break;
    case HIR_VAR_DECL:
        if (visitor->visit_var_decl != NULL) {
            visitor->visit_var_decl(visitor, node);
        }
        break;
    case HIR_TYPE_ALIAS:
        if (visitor->visit_type_alias != NULL) {
            visitor->visit_type_alias(visitor, node);
        }
        break;
    default:
        if (is_expr_kind(node->kind) && visitor->visit_expr != NULL) {
            visitor->visit_expr(visitor, node);
        } else if (is_stmt_kind(node->kind) && visitor->visit_stmt != NULL) {
            visitor->visit_stmt(visitor, node);
        }
        break;
    }

    // Walk children — delegate to the single hir_visit_children switch.
    hir_visit_children((HirNode *)node, walk_adapter, visitor);

    // Post-visit.
    if (visitor->post_visit != NULL) {
        visitor->post_visit(visitor, node);
    }
}

// ── Public API ─────────────────────────────────────────────────────────

void hir_walk(const HirNode *root, HirVisitor *visitor) {
    walk_node(root, visitor);
}
