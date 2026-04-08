#include "_lower.h"

// ── Declaration lower helpers ──────────────────────────────────────

/** Lower AST params into HIR param nodes and register them in scope. */
static void lower_param_list(Lower *low, ASTNode *const *param_asts, int32_t count,
                             HirNode ***out_params) {
    for (int32_t i = 0; i < count; i++) {
        const ASTNode *param_ast = param_asts[i];
        const char *param_name = param_ast->param.name;
        const Type *param_type = param_ast->type != NULL ? param_ast->type : &TYPE_ERR_INST;

        HirSymSpec param_spec = {HIR_SYM_PARAM, param_name, param_type, false, param_ast->loc};
        HirSym *param_sym = lower_make_sym(low, &param_spec);
        lower_scope_define(low, param_name, param_sym);

        HirNode *param_node = hir_new(low->hir_arena, HIR_PARAM, param_type, param_ast->loc);
        param_node->param.sym = param_sym;
        param_node->param.name = param_name;
        param_node->param.param_type = param_type;
        BUF_PUSH(*out_params, param_node);
    }
}

/** Lower a fn body (block or expr-bodied). */
static HirNode *lower_fn_body(Lower *low, const ASTNode *body_ast) {
    if (body_ast == NULL) {
        return NULL;
    }
    if (body_ast->kind == NODE_BLOCK) {
        return lower_block(low, body_ast);
    }
    // Expression-bodied fn: `fn f() = expr` → wrap in a block with result
    HirNode *result = lower_expr(low, body_ast);
    HirNode *body =
        hir_new(low->hir_arena, HIR_BLOCK,
                body_ast->type != NULL ? body_ast->type : &TYPE_UNIT_INST, body_ast->loc);
    body->block.result = result;
    return body;
}

// ── Declaration lower ──────────────────────────────────────────────

HirNode *lower_fn_decl(Lower *low, const ASTNode *ast) {
    const char *name = ast->fn_decl.name;
    bool is_pub = ast->fn_decl.is_pub;
    const Type *return_type = ast->type != NULL ? ast->type : &TYPE_UNIT_INST;

    HirSymSpec func_spec = {HIR_SYM_FN, name, return_type, false, ast->loc};
    HirSym *func_sym = lower_make_sym(low, &func_spec);
    func_sym->mangled_name =
        arena_sprintf(low->hir_arena, "rsgu_%s", lower_mangle_name(low->hir_arena, name));
    lower_scope_define(low, name, func_sym);

    lower_scope_enter(low);

    const Type *saved_return_type = low->fn_return_type;
    low->fn_return_type = return_type;

    HirNode **params = NULL;
    lower_param_list(low, ast->fn_decl.params, BUF_LEN(ast->fn_decl.params), &params);

    HirNode *body = lower_fn_body(low, ast->fn_decl.body);

    low->fn_return_type = saved_return_type;
    lower_scope_leave(low);

    HirNode *node = hir_new(low->hir_arena, HIR_FN_DECL, &TYPE_UNIT_INST, ast->loc);
    node->fn_decl.name = name;
    node->fn_decl.is_pub = is_pub;
    node->fn_decl.sym = func_sym;
    node->fn_decl.params = params;
    node->fn_decl.return_type = return_type;
    node->fn_decl.body = body;
    return node;
}

HirNode *lower_method_decl(Lower *low, const ASTNode *ast, const char *struct_name,
                           const Type *struct_type) {
    const char *method_name = ast->fn_decl.name;
    const Type *return_type = ast->type != NULL ? ast->type : &TYPE_UNIT_INST;
    const char *key = arena_sprintf(low->hir_arena, "%s.%s", struct_name, method_name);

    // Look up pre-registered sym
    HirSym *func_sym = lower_scope_lookup(low, key);
    if (func_sym == NULL) {
        HirSymSpec method_spec = {HIR_SYM_FN, key, return_type, false, ast->loc};
        func_sym = lower_make_sym(low, &method_spec);
        func_sym->mangled_name =
            arena_sprintf(low->hir_arena, "rsgu_%s_%s",
                          lower_mangle_name(low->hir_arena, struct_name), method_name);
        lower_scope_define(low, key, func_sym);
    }

    lower_scope_enter(low);

    // Lower recv param
    HirNode **params = NULL;
    const char *recv_name = ast->fn_decl.recv_name;
    bool is_mut_recv = ast->fn_decl.is_mut_recv;
    bool is_ptr_recv = ast->fn_decl.is_ptr_recv;

    // Save recv + fn context
    RecvCtx saved_recv = low->recv;
    const Type *saved_return_type = low->fn_return_type;
    low->fn_return_type = return_type;

    if (recv_name != NULL) {
        HirSymSpec recv_spec = {HIR_SYM_PARAM, recv_name, struct_type, false, ast->loc};
        HirSym *recv_sym = lower_make_sym(low, &recv_spec);
        recv_sym->is_ptr_recv = is_ptr_recv;
        lower_scope_define(low, recv_name, recv_sym);

        // Store is_ptr_recv on the method sym for call-site lookup
        func_sym->is_ptr_recv = is_ptr_recv;

        HirNode *recv_param = hir_new(low->hir_arena, HIR_PARAM, struct_type, ast->loc);
        recv_param->param.sym = recv_sym;
        recv_param->param.name = recv_name;
        recv_param->param.param_type = struct_type;
        recv_param->param.is_recv = true;
        recv_param->param.is_mut_recv = is_mut_recv;
        recv_param->param.is_ptr_recv = is_ptr_recv;
        BUF_PUSH(params, recv_param);

        low->recv = (RecvCtx){.sym = recv_sym, .name = recv_name, .is_ptr = is_ptr_recv};
    } else {
        func_sym->is_static = true;
    }

    // Lower other params
    lower_param_list(low, ast->fn_decl.params, BUF_LEN(ast->fn_decl.params), &params);

    HirNode *body = lower_fn_body(low, ast->fn_decl.body);

    // Restore recv + fn context
    low->recv = saved_recv;
    low->fn_return_type = saved_return_type;

    lower_scope_leave(low);

    HirNode *node = hir_new(low->hir_arena, HIR_FN_DECL, &TYPE_UNIT_INST, ast->loc);
    node->fn_decl.name = method_name;
    node->fn_decl.is_pub = false;
    node->fn_decl.sym = func_sym;
    node->fn_decl.params = params;
    node->fn_decl.return_type = return_type;
    node->fn_decl.body = body;
    return node;
}
