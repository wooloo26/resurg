#include "_lower.h"

// ── Closure capture analysis + lowering ────────────────────────────

/** Invariant context for a single capture-scanning pass. */
typedef struct {
    const char **param_names;
    int32_t param_count;
    Lower *low;
    const char ***out_names;
    HirSym ***out_syms;
} CaptureCtx;

/** Recursively scan AST for NODE_ID references that aren't closure params. */
static void scan_captures(const CaptureCtx *ctx, const ASTNode *ast) {
    if (ast == NULL) {
        return;
    }
    if (ast->kind == NODE_ID) {
        const char *name = ast->id.name;
        // Skip if it's a closure param
        for (int32_t i = 0; i < ctx->param_count; i++) {
            if (strcmp(name, ctx->param_names[i]) == 0) {
                return;
            }
        }
        // Skip if already captured
        for (int32_t i = 0; i < BUF_LEN(*ctx->out_names); i++) {
            if (strcmp(name, (*ctx->out_syms)[i]->name) == 0) {
                return;
            }
        }
        // Check if it refers to a local variable in enclosing scope
        HirSym *sym = lower_scope_lookup(ctx->low, name);
        if (sym != NULL && (sym->kind == HIR_SYM_VAR || sym->kind == HIR_SYM_PARAM)) {
            BUF_PUSH(*ctx->out_names, sym->mangled_name);
            BUF_PUSH(*ctx->out_syms, sym);
        }
        return;
    }
    if (ast->kind == NODE_CLOSURE) {
        return; // Don't scan into nested closures
    }
    // Recurse into children based on node kind
    switch (ast->kind) {
    case NODE_UNARY:
        scan_captures(ctx, ast->unary.operand);
        break;
    case NODE_BINARY:
        scan_captures(ctx, ast->binary.left);
        scan_captures(ctx, ast->binary.right);
        break;
    case NODE_CALL:
        scan_captures(ctx, ast->call.callee);
        for (int32_t i = 0; i < BUF_LEN(ast->call.args); i++) {
            scan_captures(ctx, ast->call.args[i]);
        }
        break;
    case NODE_BLOCK:
        for (int32_t i = 0; i < BUF_LEN(ast->block.stmts); i++) {
            scan_captures(ctx, ast->block.stmts[i]);
        }
        if (ast->block.result != NULL) {
            scan_captures(ctx, ast->block.result);
        }
        break;
    case NODE_IF:
        scan_captures(ctx, ast->if_expr.cond);
        scan_captures(ctx, ast->if_expr.then_body);
        scan_captures(ctx, ast->if_expr.else_body);
        break;
    case NODE_RETURN:
        scan_captures(ctx, ast->return_stmt.value);
        break;
    case NODE_VAR_DECL:
        scan_captures(ctx, ast->var_decl.init);
        break;
    case NODE_EXPR_STMT:
        scan_captures(ctx, ast->expr_stmt.expr);
        break;
    case NODE_MEMBER:
        scan_captures(ctx, ast->member.object);
        break;
    case NODE_IDX:
        scan_captures(ctx, ast->idx_access.object);
        scan_captures(ctx, ast->idx_access.idx);
        break;
    case NODE_ASSIGN:
        scan_captures(ctx, ast->assign.target);
        scan_captures(ctx, ast->assign.value);
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

    int32_t param_count = BUF_LEN(ast->closure.params);

    // Resolve captures — prefer the persisted list from check phase, fall back to tree scan.
    const char **capture_names = NULL;
    HirSym **capture_syms = NULL;
    if (ast->closure.capture_names != NULL) {
        // Use capture list persisted by check phase — no tree walk needed.
        for (int32_t i = 0; i < BUF_LEN(ast->closure.capture_names); i++) {
            HirSym *sym = lower_scope_lookup(low, ast->closure.capture_names[i]);
            if (sym != NULL && (sym->kind == HIR_SYM_VAR || sym->kind == HIR_SYM_PARAM)) {
                BUF_PUSH(capture_names, sym->mangled_name);
                BUF_PUSH(capture_syms, sym);
            }
        }
    } else {
        // Fallback: scan the AST body for captures (pre-check-phase closures).
        const char **param_names = NULL;
        for (int32_t i = 0; i < param_count; i++) {
            BUF_PUSH(param_names, ast->closure.params[i]->param.name);
        }
        CaptureCtx cap_ctx = {
            .param_names = param_names,
            .param_count = param_count,
            .low = low,
            .out_names = &capture_names,
            .out_syms = &capture_syms,
        };
        scan_captures(&cap_ctx, ast->closure.body);
        BUF_FREE(param_names);
    }

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

    return node;
}
