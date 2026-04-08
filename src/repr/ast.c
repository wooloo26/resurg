#include "repr/ast.h"

// ── Construction ──────────────────────────────────────────────────────

ASTNode *ast_new(Arena *arena, NodeKind kind, SrcLoc loc) {
    ASTNode *node = arena_alloc_zero(arena, sizeof(ASTNode));
    node->kind = kind;
    node->loc = loc;
    node->type = NULL;
    return node;
}

// ── Deep clone helpers ────────────────────────────────────────────

/** Clone a stretchy buf of ASTNode ptrs into a fresh buf. */
static ASTNode **clone_node_buf(Arena *arena, ASTNode **src_buf) {
    ASTNode **dst_buf = NULL;
    for (int32_t i = 0; i < BUF_LEN(src_buf); i++) {
        BUF_PUSH(dst_buf, ast_clone(arena, src_buf[i]));
    }
    return dst_buf;
}

/** Clone a stretchy buf of const-char ptrs (shallow copy of each str). */
static const char **clone_str_buf(const char **src_buf) {
    const char **dst_buf = NULL;
    for (int32_t i = 0; i < BUF_LEN(src_buf); i++) {
        BUF_PUSH(dst_buf, src_buf[i]);
    }
    return dst_buf;
}

/** Clone a stretchy buf of ASTType values (shallow copy of each type). */
static ASTType *clone_ast_type_buf(ASTType *src_buf) {
    ASTType *dst_buf = NULL;
    for (int32_t i = 0; i < BUF_LEN(src_buf); i++) {
        BUF_PUSH(dst_buf, src_buf[i]);
    }
    return dst_buf;
}

/** Clone a stretchy buf of bools. */
static bool *clone_bool_buf(bool *src_buf) {
    bool *dst_buf = NULL;
    for (int32_t i = 0; i < BUF_LEN(src_buf); i++) {
        BUF_PUSH(dst_buf, src_buf[i]);
    }
    return dst_buf;
}

// ── Deep clone ────────────────────────────────────────────────────

ASTNode *ast_clone(Arena *arena, ASTNode *src) {
    if (src == NULL) {
        return NULL;
    }
    ASTNode *dst = ast_new(arena, src->kind, src->loc);
    switch (src->kind) {

    // ── Leaf nodes (no child ptrs to fix up) ──────────────────────
    case NODE_LIT:
        dst->lit = src->lit;
        break;
    case NODE_ID:
        dst->id = src->id;
        break;
    case NODE_CONTINUE:
        break;
    case NODE_PARAM:
        dst->param = src->param;
        break;

    // ── Single-child nodes ────────────────────────────────────────
    case NODE_UNARY:
        dst->unary.op = src->unary.op;
        dst->unary.operand = ast_clone(arena, src->unary.operand);
        break;
    case NODE_EXPR_STMT:
        dst->expr_stmt.expr = ast_clone(arena, src->expr_stmt.expr);
        break;
    case NODE_RETURN:
        dst->return_stmt.value = ast_clone(arena, src->return_stmt.value);
        break;
    case NODE_BREAK:
        dst->break_stmt.value = ast_clone(arena, src->break_stmt.value);
        break;
    case NODE_LOOP:
        dst->loop.body = ast_clone(arena, src->loop.body);
        break;
    case NODE_DEFER:
        dst->defer_stmt.body = ast_clone(arena, src->defer_stmt.body);
        break;
    case NODE_ADDRESS_OF:
        dst->address_of.operand = ast_clone(arena, src->address_of.operand);
        break;
    case NODE_DEREF:
        dst->deref.operand = ast_clone(arena, src->deref.operand);
        break;
    case NODE_TRY:
        dst->try_expr.operand = ast_clone(arena, src->try_expr.operand);
        break;

    // ── Two-child nodes ───────────────────────────────────────────
    case NODE_BINARY:
        dst->binary.op = src->binary.op;
        dst->binary.left = ast_clone(arena, src->binary.left);
        dst->binary.right = ast_clone(arena, src->binary.right);
        break;
    case NODE_ASSIGN:
        dst->assign.target = ast_clone(arena, src->assign.target);
        dst->assign.value = ast_clone(arena, src->assign.value);
        break;
    case NODE_COMPOUND_ASSIGN:
        dst->compound_assign.op = src->compound_assign.op;
        dst->compound_assign.target = ast_clone(arena, src->compound_assign.target);
        dst->compound_assign.value = ast_clone(arena, src->compound_assign.value);
        break;
    case NODE_IDX:
        dst->idx_access.object = ast_clone(arena, src->idx_access.object);
        dst->idx_access.idx = ast_clone(arena, src->idx_access.idx);
        break;
    case NODE_MEMBER:
        dst->member.object = ast_clone(arena, src->member.object);
        dst->member.member = src->member.member;
        break;
    case NODE_OPTIONAL_CHAIN:
        dst->optional_chain.object = ast_clone(arena, src->optional_chain.object);
        dst->optional_chain.member = src->optional_chain.member;
        break;
    case NODE_WHILE:
        dst->while_loop.cond = ast_clone(arena, src->while_loop.cond);
        dst->while_loop.body = ast_clone(arena, src->while_loop.body);
        dst->while_loop.pattern = src->while_loop.pattern; // patterns are not deep-cloned
        dst->while_loop.pattern_init = ast_clone(arena, src->while_loop.pattern_init);
        break;
    case NODE_TYPE_CONVERSION:
        dst->type_conversion.target_type = src->type_conversion.target_type;
        dst->type_conversion.operand = ast_clone(arena, src->type_conversion.operand);
        break;

    // ── Multi-child / buf nodes ───────────────────────────────────
    case NODE_BLOCK:
        dst->block.stmts = clone_node_buf(arena, src->block.stmts);
        dst->block.result = ast_clone(arena, src->block.result);
        break;
    case NODE_IF:
        dst->if_expr.cond = ast_clone(arena, src->if_expr.cond);
        dst->if_expr.then_body = ast_clone(arena, src->if_expr.then_body);
        dst->if_expr.else_body = ast_clone(arena, src->if_expr.else_body);
        dst->if_expr.pattern = src->if_expr.pattern;
        dst->if_expr.pattern_init = ast_clone(arena, src->if_expr.pattern_init);
        break;
    case NODE_VAR_DECL:
        dst->var_decl = src->var_decl;
        dst->var_decl.init = ast_clone(arena, src->var_decl.init);
        break;
    case NODE_CALL:
        dst->call.callee = ast_clone(arena, src->call.callee);
        dst->call.args = clone_node_buf(arena, src->call.args);
        dst->call.arg_names = clone_str_buf(src->call.arg_names);
        dst->call.arg_is_mut = clone_bool_buf(src->call.arg_is_mut);
        dst->call.type_args = clone_ast_type_buf(src->call.type_args);
        break;
    case NODE_FOR:
        dst->for_loop = src->for_loop;
        dst->for_loop.start = ast_clone(arena, src->for_loop.start);
        dst->for_loop.end = ast_clone(arena, src->for_loop.end);
        dst->for_loop.iterable = ast_clone(arena, src->for_loop.iterable);
        dst->for_loop.body = ast_clone(arena, src->for_loop.body);
        break;
    case NODE_STR_INTERPOLATION:
        dst->str_interpolation.parts = clone_node_buf(arena, src->str_interpolation.parts);
        break;
    case NODE_ARRAY_LIT:
        dst->array_lit = src->array_lit;
        dst->array_lit.elems = clone_node_buf(arena, src->array_lit.elems);
        break;
    case NODE_SLICE_LIT:
        dst->slice_lit = src->slice_lit;
        dst->slice_lit.elems = clone_node_buf(arena, src->slice_lit.elems);
        break;
    case NODE_SLICE_EXPR:
        dst->slice_expr.object = ast_clone(arena, src->slice_expr.object);
        dst->slice_expr.start = ast_clone(arena, src->slice_expr.start);
        dst->slice_expr.end = ast_clone(arena, src->slice_expr.end);
        dst->slice_expr.full_range = src->slice_expr.full_range;
        break;
    case NODE_TUPLE_LIT:
        dst->tuple_lit.elems = clone_node_buf(arena, src->tuple_lit.elems);
        break;
    case NODE_STRUCT_LIT:
        dst->struct_lit.name = src->struct_lit.name;
        dst->struct_lit.field_names = clone_str_buf(src->struct_lit.field_names);
        dst->struct_lit.field_values = clone_node_buf(arena, src->struct_lit.field_values);
        dst->struct_lit.type_args = clone_ast_type_buf(src->struct_lit.type_args);
        break;
    case NODE_STRUCT_DESTRUCTURE:
        dst->struct_destructure.field_names = clone_str_buf(src->struct_destructure.field_names);
        dst->struct_destructure.aliases = clone_str_buf(src->struct_destructure.aliases);
        dst->struct_destructure.value = ast_clone(arena, src->struct_destructure.value);
        break;
    case NODE_TUPLE_DESTRUCTURE:
        dst->tuple_destructure.names = clone_str_buf(src->tuple_destructure.names);
        dst->tuple_destructure.value = ast_clone(arena, src->tuple_destructure.value);
        dst->tuple_destructure.has_rest = src->tuple_destructure.has_rest;
        dst->tuple_destructure.rest_pos = src->tuple_destructure.rest_pos;
        break;
    case NODE_CLOSURE:
        dst->closure.return_type = src->closure.return_type;
        dst->closure.params = clone_node_buf(arena, src->closure.params);
        dst->closure.body = ast_clone(arena, src->closure.body);
        break;
    case NODE_MATCH:
        dst->match_expr.operand = ast_clone(arena, src->match_expr.operand);
        dst->match_expr.arms = NULL;
        for (int32_t i = 0; i < BUF_LEN(src->match_expr.arms); i++) {
            ASTMatchArm arm = src->match_expr.arms[i];
            arm.body = ast_clone(arena, arm.body);
            arm.guard = ast_clone(arena, arm.guard);
            BUF_PUSH(dst->match_expr.arms, arm);
        }
        break;
    case NODE_ENUM_INIT:
        dst->enum_init.enum_name = src->enum_init.enum_name;
        dst->enum_init.variant_name = src->enum_init.variant_name;
        dst->enum_init.args = clone_node_buf(arena, src->enum_init.args);
        dst->enum_init.field_names = clone_str_buf(src->enum_init.field_names);
        dst->enum_init.field_values = clone_node_buf(arena, src->enum_init.field_values);
        dst->enum_init.type_args = clone_ast_type_buf(src->enum_init.type_args);
        break;

    // ── Decl nodes (top-level; clone bufs to avoid sharing) ───────
    case NODE_FN_DECL:
        dst->fn_decl = src->fn_decl;
        dst->fn_decl.params = clone_node_buf(arena, src->fn_decl.params);
        dst->fn_decl.body = ast_clone(arena, src->fn_decl.body);
        break;
    case NODE_FILE:
        dst->file.decls = clone_node_buf(arena, src->file.decls);
        break;
    case NODE_MODULE:
        dst->module = src->module;
        dst->module.decls = clone_node_buf(arena, src->module.decls);
        break;
    case NODE_TYPE_ALIAS:
        dst->type_alias = src->type_alias;
        break;
    case NODE_USE_DECL:
        dst->use_decl = src->use_decl;
        dst->use_decl.imported_names = clone_str_buf(src->use_decl.imported_names);
        dst->use_decl.aliases = clone_str_buf(src->use_decl.aliases);
        break;

    // Struct/enum/pact/ext decls are not cloned during monomorphization
    // of fn bodies.  Fatal here to catch unexpected usage.
    case NODE_STRUCT_DECL:
    case NODE_ENUM_DECL:
    case NODE_PACT_DECL:
    case NODE_EXT_DECL:
        rsg_fatal("ast_clone: unsupported top-level decl node kind %d", (int)src->kind);
    }
    return dst;
}
