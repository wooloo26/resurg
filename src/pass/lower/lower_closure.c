#include "_lower.h"

// ── Closure capture analysis + lowering ────────────────────────────

/** Recursively scan AST for NODE_ID references that aren't closure params. */
static void scan_captures(const ASTNode *ast, const char **param_names, int32_t param_count,
                          Lower *low, const char ***out_names, HirSym ***out_syms) {
    if (ast == NULL) {
        return;
    }
    if (ast->kind == NODE_ID) {
        const char *name = ast->id.name;
        // Skip if it's a closure param
        for (int32_t i = 0; i < param_count; i++) {
            if (strcmp(name, param_names[i]) == 0) {
                return;
            }
        }
        // Skip if already captured
        for (int32_t i = 0; i < BUF_LEN(*out_names); i++) {
            if (strcmp(name, (*out_syms)[i]->name) == 0) {
                return;
            }
        }
        // Check if it refers to a local variable in enclosing scope
        HirSym *sym = lower_scope_lookup(low, name);
        if (sym != NULL && (sym->kind == HIR_SYM_VAR || sym->kind == HIR_SYM_PARAM)) {
            BUF_PUSH(*out_names, sym->mangled_name);
            BUF_PUSH(*out_syms, sym);
        }
        return;
    }
    if (ast->kind == NODE_CLOSURE) {
        return; // Don't scan into nested closures
    }
    // Recurse into children based on node kind
    switch (ast->kind) {
    case NODE_UNARY:
        scan_captures(ast->unary.operand, param_names, param_count, low, out_names, out_syms);
        break;
    case NODE_BINARY:
        scan_captures(ast->binary.left, param_names, param_count, low, out_names, out_syms);
        scan_captures(ast->binary.right, param_names, param_count, low, out_names, out_syms);
        break;
    case NODE_CALL:
        scan_captures(ast->call.callee, param_names, param_count, low, out_names, out_syms);
        for (int32_t i = 0; i < BUF_LEN(ast->call.args); i++) {
            scan_captures(ast->call.args[i], param_names, param_count, low, out_names, out_syms);
        }
        break;
    case NODE_BLOCK:
        for (int32_t i = 0; i < BUF_LEN(ast->block.stmts); i++) {
            scan_captures(ast->block.stmts[i], param_names, param_count, low, out_names, out_syms);
        }
        if (ast->block.result != NULL) {
            scan_captures(ast->block.result, param_names, param_count, low, out_names, out_syms);
        }
        break;
    case NODE_IF:
        scan_captures(ast->if_expr.cond, param_names, param_count, low, out_names, out_syms);
        scan_captures(ast->if_expr.then_body, param_names, param_count, low, out_names, out_syms);
        scan_captures(ast->if_expr.else_body, param_names, param_count, low, out_names, out_syms);
        break;
    case NODE_RETURN:
        scan_captures(ast->return_stmt.value, param_names, param_count, low, out_names, out_syms);
        break;
    case NODE_VAR_DECL:
        scan_captures(ast->var_decl.init, param_names, param_count, low, out_names, out_syms);
        break;
    case NODE_EXPR_STMT:
        scan_captures(ast->expr_stmt.expr, param_names, param_count, low, out_names, out_syms);
        break;
    case NODE_MEMBER:
        scan_captures(ast->member.object, param_names, param_count, low, out_names, out_syms);
        break;
    case NODE_IDX:
        scan_captures(ast->idx_access.object, param_names, param_count, low, out_names, out_syms);
        scan_captures(ast->idx_access.idx, param_names, param_count, low, out_names, out_syms);
        break;
    case NODE_ASSIGN:
        scan_captures(ast->assign.target, param_names, param_count, low, out_names, out_syms);
        scan_captures(ast->assign.value, param_names, param_count, low, out_names, out_syms);
        break;
    case NODE_LIT: // NOLINT(bugprone-branch-clone)
    case NODE_STR_INTERPOLATION:
        break;
    default:
        break;
    }
}

HirNode *lower_closure(Lower *low, const ASTNode *ast) {
    SrcLoc loc = ast->loc;
    const Type *fn_type = ast->type;

    // Generate a unique name for the closure function
    const char *fn_name = arena_sprintf(low->hir_arena, "_rsg_closure_%d", low->closure_counter++);

    // Collect closure param names for capture scanner
    int32_t param_count = BUF_LEN(ast->closure.params);
    const char **param_names = NULL;
    for (int32_t i = 0; i < param_count; i++) {
        BUF_PUSH(param_names, ast->closure.params[i]->param.name);
    }

    // Scan for captured variables
    const char **capture_names = NULL;
    HirSym **capture_syms = NULL;
    scan_captures(ast->closure.body, param_names, param_count, low, &capture_names, &capture_syms);

    // Lower closure params to HIR_PARAM
    HirNode **hir_params = NULL;
    lower_scope_enter(low);
    for (int32_t i = 0; i < param_count; i++) {
        const ASTNode *p = ast->closure.params[i];
        HirSymSpec spec = {HIR_SYM_PARAM, p->param.name, p->type, p->param.is_mut, p->loc};
        HirSym *sym = lower_make_sym(low, &spec);
        lower_scope_define(low, p->param.name, sym);
        HirNode *hir_param = hir_new(low->hir_arena, HIR_PARAM, p->type, p->loc);
        hir_param->param.sym = sym;
        BUF_PUSH(hir_params, hir_param);
    }

    // Define captured vars in closure scope so body can reference them
    for (int32_t i = 0; i < BUF_LEN(capture_syms); i++) {
        lower_scope_define(low, capture_syms[i]->name, capture_syms[i]);
    }

    // Lower closure body
    HirNode *body = lower_expr(low, ast->closure.body);
    lower_scope_leave(low);

    // Build HIR_CLOSURE
    const Type *return_type = fn_type->fn_type.return_type;
    HirNode *node = hir_new(low->hir_arena, HIR_CLOSURE, fn_type, loc);
    node->closure.fn_name = fn_name;
    node->closure.params = hir_params;
    node->closure.body = body;
    node->closure.capture_names = capture_names;
    node->closure.capture_syms = capture_syms;
    node->closure.return_type = return_type;
    node->closure.is_fn_mut = (fn_type->fn_type.fn_kind == FN_CLOSURE_MUT);

    BUF_FREE(param_names);
    return node;
}
