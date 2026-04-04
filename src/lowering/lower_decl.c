#include "_lowering.h"

// ── Declaration lowering ──────────────────────────────────────────────

TtNode *lower_function_declaration(Lowering *low, const ASTNode *ast) {
    const char *name = ast->function_declaration.name;
    bool is_public = ast->function_declaration.is_public;
    const Type *return_type = ast->type != NULL ? ast->type : &TYPE_UNIT_INSTANCE;

    TtSymbol *func_sym =
        lowering_make_symbol(low, TT_SYMBOL_FUNCTION, name, return_type, false, ast->location);
    func_sym->mangled_name = arena_sprintf(low->tt_arena, "rsgu_%s", name);
    lowering_scope_add(low, name, func_sym);

    lowering_scope_enter(low);

    // Lower parameters
    TtNode **params = NULL;
    for (int32_t i = 0; i < BUFFER_LENGTH(ast->function_declaration.parameters); i++) {
        const ASTNode *param_ast = ast->function_declaration.parameters[i];
        const char *param_name = param_ast->parameter.name;
        const Type *param_type = param_ast->type != NULL ? param_ast->type : &TYPE_ERROR_INSTANCE;

        TtSymbol *param_sym = lowering_make_symbol(low, TT_SYMBOL_PARAMETER, param_name, param_type,
                                                   false, param_ast->location);
        lowering_scope_add(low, param_name, param_sym);

        TtNode *param_node = tt_new(low->tt_arena, TT_PARAMETER, param_type, param_ast->location);
        param_node->parameter.symbol = param_sym;
        param_node->parameter.name = param_name;
        param_node->parameter.param_type = param_type;
        BUFFER_PUSH(params, param_node);
    }

    // Lower body
    TtNode *body = NULL;
    if (ast->function_declaration.body != NULL) {
        if (ast->function_declaration.body->kind == NODE_BLOCK) {
            body = lower_block(low, ast->function_declaration.body);
        } else {
            // Expression-bodied function: `fn f() = expr` → wrap in a block with result
            TtNode *result = lower_expression(low, ast->function_declaration.body);
            body = tt_new(low->tt_arena, TT_BLOCK,
                          ast->function_declaration.body->type != NULL
                              ? ast->function_declaration.body->type
                              : &TYPE_UNIT_INSTANCE,
                          ast->function_declaration.body->location);
            body->block.result = result;
        }
    }

    lowering_scope_leave(low);

    TtNode *node =
        tt_new(low->tt_arena, TT_FUNCTION_DECLARATION, &TYPE_UNIT_INSTANCE, ast->location);
    node->function_declaration.name = name;
    node->function_declaration.is_public = is_public;
    node->function_declaration.symbol = func_sym;
    node->function_declaration.params = params;
    node->function_declaration.return_type = return_type;
    node->function_declaration.body = body;
    return node;
}

TtNode *lower_method_declaration(Lowering *low, const ASTNode *ast, const char *struct_name,
                                 const Type *struct_type) {
    const char *method_name = ast->function_declaration.name;
    const Type *return_type = ast->type != NULL ? ast->type : &TYPE_UNIT_INSTANCE;
    const char *key = arena_sprintf(low->tt_arena, "%s.%s", struct_name, method_name);

    // Look up pre-registered symbol
    TtSymbol *func_sym = lowering_scope_find(low, key);
    if (func_sym == NULL) {
        func_sym =
            lowering_make_symbol(low, TT_SYMBOL_FUNCTION, key, return_type, false, ast->location);
        func_sym->mangled_name =
            arena_sprintf(low->tt_arena, "rsgu_%s_%s", struct_name, method_name);
        lowering_scope_add(low, key, func_sym);
    }

    lowering_scope_enter(low);

    // Lower receiver parameter
    TtNode **params = NULL;
    const char *receiver_name = ast->function_declaration.receiver_name;
    bool is_mut_receiver = ast->function_declaration.is_mut_receiver;

    TtSymbol *recv_sym = lowering_make_symbol(low, TT_SYMBOL_PARAMETER, receiver_name, struct_type,
                                              false, ast->location);
    lowering_scope_add(low, receiver_name, recv_sym);

    TtNode *recv_param = tt_new(low->tt_arena, TT_PARAMETER, struct_type, ast->location);
    recv_param->parameter.symbol = recv_sym;
    recv_param->parameter.name = receiver_name;
    recv_param->parameter.param_type = struct_type;
    recv_param->parameter.is_receiver = true;
    recv_param->parameter.is_mut_receiver = is_mut_receiver;
    BUFFER_PUSH(params, recv_param);

    // Set current receiver for via_pointer detection
    TtSymbol *saved_receiver = low->current_receiver;
    const char *saved_name = low->current_receiver_name;
    low->current_receiver = recv_sym;
    low->current_receiver_name = receiver_name;

    // Lower other parameters
    for (int32_t i = 0; i < BUFFER_LENGTH(ast->function_declaration.parameters); i++) {
        const ASTNode *param_ast = ast->function_declaration.parameters[i];
        const char *param_name = param_ast->parameter.name;
        const Type *param_type = param_ast->type != NULL ? param_ast->type : &TYPE_ERROR_INSTANCE;

        TtSymbol *param_sym = lowering_make_symbol(low, TT_SYMBOL_PARAMETER, param_name, param_type,
                                                   false, param_ast->location);
        lowering_scope_add(low, param_name, param_sym);

        TtNode *param_node = tt_new(low->tt_arena, TT_PARAMETER, param_type, param_ast->location);
        param_node->parameter.symbol = param_sym;
        param_node->parameter.name = param_name;
        param_node->parameter.param_type = param_type;
        BUFFER_PUSH(params, param_node);
    }

    // Lower body
    TtNode *body = NULL;
    if (ast->function_declaration.body != NULL) {
        if (ast->function_declaration.body->kind == NODE_BLOCK) {
            body = lower_block(low, ast->function_declaration.body);
        } else {
            TtNode *result = lower_expression(low, ast->function_declaration.body);
            body = tt_new(low->tt_arena, TT_BLOCK,
                          ast->function_declaration.body->type != NULL
                              ? ast->function_declaration.body->type
                              : &TYPE_UNIT_INSTANCE,
                          ast->function_declaration.body->location);
            body->block.result = result;
        }
    }

    // Restore receiver context
    low->current_receiver = saved_receiver;
    low->current_receiver_name = saved_name;

    lowering_scope_leave(low);

    TtNode *node =
        tt_new(low->tt_arena, TT_FUNCTION_DECLARATION, &TYPE_UNIT_INSTANCE, ast->location);
    node->function_declaration.name = method_name;
    node->function_declaration.is_public = false;
    node->function_declaration.symbol = func_sym;
    node->function_declaration.params = params;
    node->function_declaration.return_type = return_type;
    node->function_declaration.body = body;
    return node;
}
