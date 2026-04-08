#include "repr/ast.h"

// ── Construction ──────────────────────────────────────────────────────

ASTNode *ast_new(Arena *arena, NodeKind kind, SrcLoc loc) {
    ASTNode *node = arena_alloc_zero(arena, sizeof(ASTNode));
    node->kind = kind;
    node->loc = loc;
    node->type = NULL;
    return node;
}

// ── Deep clone ────────────────────────────────────────────────────

ASTNode *ast_clone(Arena *arena, ASTNode *src) {
    if (src == NULL) {
        return NULL;
    }
    ASTNode *dst = ast_new(arena, src->kind, src->loc);
    switch (src->kind) {
    case NODE_LIT:
        dst->lit = src->lit;
        break;
    case NODE_ID:
        dst->id = src->id;
        break;
    case NODE_UNARY:
        dst->unary.op = src->unary.op;
        dst->unary.operand = ast_clone(arena, src->unary.operand);
        break;
    case NODE_BINARY:
        dst->binary.op = src->binary.op;
        dst->binary.left = ast_clone(arena, src->binary.left);
        dst->binary.right = ast_clone(arena, src->binary.right);
        break;
    case NODE_BLOCK:
        dst->block.stmts = NULL;
        for (int32_t i = 0; i < BUF_LEN(src->block.stmts); i++) {
            BUF_PUSH(dst->block.stmts, ast_clone(arena, src->block.stmts[i]));
        }
        dst->block.result = ast_clone(arena, src->block.result);
        break;
    case NODE_EXPR_STMT:
        dst->expr_stmt.expr = ast_clone(arena, src->expr_stmt.expr);
        break;
    case NODE_VAR_DECL:
        dst->var_decl = src->var_decl;
        dst->var_decl.init = ast_clone(arena, src->var_decl.init);
        break;
    case NODE_CALL:
        dst->call.callee = ast_clone(arena, src->call.callee);
        dst->call.args = NULL;
        dst->call.arg_names = NULL;
        dst->call.arg_is_mut = NULL;
        dst->call.type_args = NULL;
        for (int32_t i = 0; i < BUF_LEN(src->call.args); i++) {
            BUF_PUSH(dst->call.args, ast_clone(arena, src->call.args[i]));
        }
        for (int32_t i = 0; i < BUF_LEN(src->call.arg_names); i++) {
            BUF_PUSH(dst->call.arg_names, src->call.arg_names[i]);
        }
        for (int32_t i = 0; i < BUF_LEN(src->call.arg_is_mut); i++) {
            BUF_PUSH(dst->call.arg_is_mut, src->call.arg_is_mut[i]);
        }
        break;
    case NODE_MEMBER:
        dst->member.object = ast_clone(arena, src->member.object);
        dst->member.member = src->member.member;
        break;
    case NODE_IDX:
        dst->idx_access.object = ast_clone(arena, src->idx_access.object);
        dst->idx_access.idx = ast_clone(arena, src->idx_access.idx);
        break;
    case NODE_IF:
        dst->if_expr.cond = ast_clone(arena, src->if_expr.cond);
        dst->if_expr.then_body = ast_clone(arena, src->if_expr.then_body);
        dst->if_expr.else_body = ast_clone(arena, src->if_expr.else_body);
        break;
    case NODE_RETURN:
        dst->return_stmt.value = ast_clone(arena, src->return_stmt.value);
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
    case NODE_STR_INTERPOLATION:
        dst->str_interpolation.parts = NULL;
        for (int32_t i = 0; i < BUF_LEN(src->str_interpolation.parts); i++) {
            BUF_PUSH(dst->str_interpolation.parts,
                     ast_clone(arena, src->str_interpolation.parts[i]));
        }
        break;
    case NODE_TUPLE_LIT:
        dst->tuple_lit.elems = NULL;
        for (int32_t i = 0; i < BUF_LEN(src->tuple_lit.elems); i++) {
            BUF_PUSH(dst->tuple_lit.elems, ast_clone(arena, src->tuple_lit.elems[i]));
        }
        break;
    case NODE_ARRAY_LIT:
        dst->array_lit = src->array_lit;
        dst->array_lit.elems = NULL;
        for (int32_t i = 0; i < BUF_LEN(src->array_lit.elems); i++) {
            BUF_PUSH(dst->array_lit.elems, ast_clone(arena, src->array_lit.elems[i]));
        }
        break;
    case NODE_STRUCT_LIT:
        dst->struct_lit.name = src->struct_lit.name;
        dst->struct_lit.field_names = NULL;
        dst->struct_lit.field_values = NULL;
        dst->struct_lit.type_args = NULL;
        for (int32_t i = 0; i < BUF_LEN(src->struct_lit.field_names); i++) {
            BUF_PUSH(dst->struct_lit.field_names, src->struct_lit.field_names[i]);
        }
        for (int32_t i = 0; i < BUF_LEN(src->struct_lit.field_values); i++) {
            BUF_PUSH(dst->struct_lit.field_values,
                     ast_clone(arena, src->struct_lit.field_values[i]));
        }
        for (int32_t i = 0; i < BUF_LEN(src->struct_lit.type_args); i++) {
            BUF_PUSH(dst->struct_lit.type_args, src->struct_lit.type_args[i]);
        }
        break;
    case NODE_ADDRESS_OF:
        dst->address_of.operand = ast_clone(arena, src->address_of.operand);
        break;
    case NODE_DEREF:
        dst->deref.operand = ast_clone(arena, src->deref.operand);
        break;
    case NODE_TYPE_CONVERSION:
        dst->type_conversion.target_type = src->type_conversion.target_type;
        dst->type_conversion.operand = ast_clone(arena, src->type_conversion.operand);
        break;
    case NODE_LOOP:
        dst->loop.body = ast_clone(arena, src->loop.body);
        break;
    case NODE_WHILE:
        dst->while_loop.cond = ast_clone(arena, src->while_loop.cond);
        dst->while_loop.body = ast_clone(arena, src->while_loop.body);
        break;
    case NODE_FOR:
        dst->for_loop = src->for_loop;
        dst->for_loop.start = ast_clone(arena, src->for_loop.start);
        dst->for_loop.end = ast_clone(arena, src->for_loop.end);
        dst->for_loop.iterable = ast_clone(arena, src->for_loop.iterable);
        dst->for_loop.body = ast_clone(arena, src->for_loop.body);
        break;
    case NODE_BREAK:
        dst->break_stmt.value = ast_clone(arena, src->break_stmt.value);
        break;
    case NODE_CONTINUE:
        break;
    case NODE_DEFER:
        dst->defer_stmt.body = ast_clone(arena, src->defer_stmt.body);
        break;
    case NODE_PARAM:
        dst->param = src->param;
        break;
    case NODE_CLOSURE:
        dst->closure.return_type = src->closure.return_type;
        dst->closure.params = NULL;
        for (int32_t i = 0; i < BUF_LEN(src->closure.params); i++) {
            BUF_PUSH(dst->closure.params, ast_clone(arena, src->closure.params[i]));
        }
        dst->closure.body = ast_clone(arena, src->closure.body);
        break;
    case NODE_FN_DECL:
        dst->fn_decl = src->fn_decl;
        dst->fn_decl.params = NULL;
        for (int32_t i = 0; i < BUF_LEN(src->fn_decl.params); i++) {
            BUF_PUSH(dst->fn_decl.params, ast_clone(arena, src->fn_decl.params[i]));
        }
        dst->fn_decl.body = ast_clone(arena, src->fn_decl.body);
        break;
    default:
        // Shallow copy for any unhandled kinds
        *dst = *src;
        dst->type = NULL;
        break;
    }
    return dst;
}
