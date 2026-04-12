#include "repr/ast_visitor.h"

/**
 * @file ast_visitor.c
 * @brief AST visitor framework — generic tree walker with per-kind dispatch.
 */

// ── Node classification helpers ────────────────────────────────────────

/** Return true if @p kind is an expression node. */
static bool is_expr_kind(NodeKind kind) {
    switch (kind) {
    case NODE_LIT:
    case NODE_ID:
    case NODE_UNARY:
    case NODE_BINARY:
    case NODE_CALL:
    case NODE_MEMBER:
    case NODE_IDX:
    case NODE_IF:
    case NODE_LOOP:
    case NODE_WHILE:
    case NODE_FOR:
    case NODE_BLOCK:
    case NODE_STR_INTERPOLATION:
    case NODE_ARRAY_LIT:
    case NODE_SLICE_LIT:
    case NODE_SLICE_EXPR:
    case NODE_TUPLE_LIT:
    case NODE_TYPE_CONVERSION:
    case NODE_STRUCT_LIT:
    case NODE_ADDRESS_OF:
    case NODE_DEREF:
    case NODE_MATCH:
    case NODE_ENUM_INIT:
    case NODE_OPTIONAL_CHAIN:
    case NODE_TRY:
    case NODE_CLOSURE:
        return true;
    default:
        return false;
    }
}

/** Return true if @p kind is a statement node. */
static bool is_stmt_kind(NodeKind kind) {
    switch (kind) {
    case NODE_EXPR_STMT:
    case NODE_RETURN:
    case NODE_BREAK:
    case NODE_CONTINUE:
    case NODE_DEFER:
    case NODE_ASSIGN:
    case NODE_COMPOUND_ASSIGN:
    case NODE_STRUCT_DESTRUCTURE:
    case NODE_TUPLE_DESTRUCTURE:
        return true;
    default:
        return false;
    }
}

// ── Child iteration ────────────────────────────────────────────────────

/** Visit a stretchy buf of child ptrs. */
static void visit_buf(void (*visit)(const ASTNode *, ASTVisitor *), ASTVisitor *v, ASTNode **buf,
                      int32_t count) {
    for (int32_t i = 0; i < count; i++) {
        visit(buf[i], v);
    }
}

// ── Recursive walk ─────────────────────────────────────────────────────

static void walk_node(const ASTNode *node, ASTVisitor *visitor);

static void walk_node(const ASTNode *node, ASTVisitor *visitor) {
    if (node == NULL) {
        return;
    }

    // Pre-visit gate.
    if (visitor->pre_visit != NULL) {
        if (!visitor->pre_visit(visitor, node)) {
            return;
        }
    }

    // Per-category dispatch.
    switch (node->kind) {
    case NODE_FN_DECL:
        if (visitor->visit_fn_decl != NULL) {
            visitor->visit_fn_decl(visitor, node);
        }
        break;
    case NODE_STRUCT_DECL:
        if (visitor->visit_struct_decl != NULL) {
            visitor->visit_struct_decl(visitor, node);
        }
        break;
    case NODE_ENUM_DECL:
        if (visitor->visit_enum_decl != NULL) {
            visitor->visit_enum_decl(visitor, node);
        }
        break;
    case NODE_VAR_DECL:
        if (visitor->visit_var_decl != NULL) {
            visitor->visit_var_decl(visitor, node);
        }
        break;
    case NODE_TYPE_ALIAS:
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

    // Walk children.
    switch (node->kind) {
    case NODE_FILE:
        visit_buf(walk_node, visitor, node->file.decls, BUF_LEN(node->file.decls));
        break;
    case NODE_MODULE:
        visit_buf(walk_node, visitor, node->module.decls, BUF_LEN(node->module.decls));
        break;
    case NODE_FN_DECL:
        visit_buf(walk_node, visitor, node->fn_decl.params, BUF_LEN(node->fn_decl.params));
        walk_node(node->fn_decl.body, visitor);
        break;
    case NODE_VAR_DECL:
        walk_node(node->var_decl.init, visitor);
        break;
    case NODE_EXPR_STMT:
        walk_node(node->expr_stmt.expr, visitor);
        break;
    case NODE_UNARY:
        walk_node(node->unary.operand, visitor);
        break;
    case NODE_BINARY:
        walk_node(node->binary.left, visitor);
        walk_node(node->binary.right, visitor);
        break;
    case NODE_ASSIGN:
        walk_node(node->assign.target, visitor);
        walk_node(node->assign.value, visitor);
        break;
    case NODE_COMPOUND_ASSIGN:
        walk_node(node->compound_assign.target, visitor);
        walk_node(node->compound_assign.value, visitor);
        break;
    case NODE_CALL:
        walk_node(node->call.callee, visitor);
        visit_buf(walk_node, visitor, node->call.args, BUF_LEN(node->call.args));
        break;
    case NODE_MEMBER:
        walk_node(node->member.object, visitor);
        break;
    case NODE_IDX:
        walk_node(node->idx_access.object, visitor);
        walk_node(node->idx_access.idx, visitor);
        break;
    case NODE_IF:
        walk_node(node->if_expr.pattern_init, visitor);
        walk_node(node->if_expr.cond, visitor);
        walk_node(node->if_expr.then_body, visitor);
        walk_node(node->if_expr.else_body, visitor);
        break;
    case NODE_LOOP:
        walk_node(node->loop.body, visitor);
        break;
    case NODE_WHILE:
        walk_node(node->while_loop.pattern_init, visitor);
        walk_node(node->while_loop.cond, visitor);
        walk_node(node->while_loop.body, visitor);
        break;
    case NODE_FOR:
        walk_node(node->for_loop.start, visitor);
        walk_node(node->for_loop.end, visitor);
        walk_node(node->for_loop.iterable, visitor);
        walk_node(node->for_loop.body, visitor);
        break;
    case NODE_BLOCK:
        visit_buf(walk_node, visitor, node->block.stmts, BUF_LEN(node->block.stmts));
        walk_node(node->block.result, visitor);
        break;
    case NODE_STR_INTERPOLATION:
        visit_buf(walk_node, visitor, node->str_interpolation.parts,
                  BUF_LEN(node->str_interpolation.parts));
        break;
    case NODE_ARRAY_LIT:
        visit_buf(walk_node, visitor, node->array_lit.elems, BUF_LEN(node->array_lit.elems));
        break;
    case NODE_SLICE_LIT:
        visit_buf(walk_node, visitor, node->slice_lit.elems, BUF_LEN(node->slice_lit.elems));
        break;
    case NODE_SLICE_EXPR:
        walk_node(node->slice_expr.object, visitor);
        walk_node(node->slice_expr.start, visitor);
        walk_node(node->slice_expr.end, visitor);
        break;
    case NODE_TUPLE_LIT:
        visit_buf(walk_node, visitor, node->tuple_lit.elems, BUF_LEN(node->tuple_lit.elems));
        break;
    case NODE_TYPE_CONVERSION:
        walk_node(node->type_conversion.operand, visitor);
        break;
    case NODE_STRUCT_LIT:
        visit_buf(walk_node, visitor, node->struct_lit.field_values,
                  BUF_LEN(node->struct_lit.field_values));
        break;
    case NODE_STRUCT_DESTRUCTURE:
        walk_node(node->struct_destructure.value, visitor);
        break;
    case NODE_TUPLE_DESTRUCTURE:
        walk_node(node->tuple_destructure.value, visitor);
        break;
    case NODE_ADDRESS_OF:
        walk_node(node->address_of.operand, visitor);
        break;
    case NODE_DEREF:
        walk_node(node->deref.operand, visitor);
        break;
    case NODE_OPTIONAL_CHAIN:
        walk_node(node->optional_chain.object, visitor);
        break;
    case NODE_TRY:
        walk_node(node->try_expr.operand, visitor);
        break;
    case NODE_MATCH:
        walk_node(node->match_expr.operand, visitor);
        for (int32_t i = 0; i < BUF_LEN(node->match_expr.arms); i++) {
            walk_node(node->match_expr.arms[i].body, visitor);
            walk_node(node->match_expr.arms[i].guard, visitor);
        }
        break;
    case NODE_ENUM_INIT:
        visit_buf(walk_node, visitor, node->enum_init.args, BUF_LEN(node->enum_init.args));
        visit_buf(walk_node, visitor, node->enum_init.field_values,
                  BUF_LEN(node->enum_init.field_values));
        break;
    case NODE_RETURN:
        walk_node(node->return_stmt.value, visitor);
        break;
    case NODE_DEFER:
        walk_node(node->defer_stmt.body, visitor);
        break;
    case NODE_BREAK:
        walk_node(node->break_stmt.value, visitor);
        break;
    case NODE_CLOSURE:
        visit_buf(walk_node, visitor, node->closure.params, BUF_LEN(node->closure.params));
        walk_node(node->closure.body, visitor);
        break;
    default:
        // Leaf nodes: NODE_LIT, NODE_ID, NODE_PARAM, NODE_CONTINUE,
        // NODE_TYPE_ALIAS, NODE_USE_DECL, NODE_PACT_DECL, NODE_EXT_DECL,
        // NODE_STRUCT_DECL, NODE_ENUM_DECL
        break;
    }

    // Post-visit.
    if (visitor->post_visit != NULL) {
        visitor->post_visit(visitor, node);
    }
}

// ── Public API ─────────────────────────────────────────────────────────

void ast_walk(const ASTNode *root, ASTVisitor *visitor) {
    walk_node(root, visitor);
}
