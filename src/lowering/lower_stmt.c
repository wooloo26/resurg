#include "_lowering.h"

// ── Statement / control-flow lowering ─────────────────────────────────

TtNode *lower_statement_if(Lowering *low, const ASTNode *ast) {
    TtNode *condition = lower_expression(low, ast->if_expression.condition);
    TtNode *then_body = lower_node(low, ast->if_expression.then_body);
    TtNode *else_body = NULL;
    if (ast->if_expression.else_body != NULL) {
        else_body = lower_node(low, ast->if_expression.else_body);
    }
    TtNode *node = tt_new(low->tt_arena, TT_IF, ast->type, ast->location);
    node->if_expression.condition = condition;
    node->if_expression.then_body = then_body;
    node->if_expression.else_body = else_body;
    return node;
}

static TtNode *lower_variable_declaration(Lowering *low, const ASTNode *ast) {
    const char *name = ast->variable_declaration.name;
    const Type *type = ast->type != NULL ? ast->type : &TYPE_ERROR_INSTANCE;
    bool is_mut = ast->variable_declaration.is_variable;

    TtSymbol *symbol =
        lowering_make_symbol(low, TT_SYMBOL_VARIABLE, name, type, is_mut, ast->location);
    lowering_scope_add(low, name, symbol);

    TtNode *init = NULL;
    if (ast->variable_declaration.initializer != NULL) {
        init = lower_expression(low, ast->variable_declaration.initializer);
    }

    TtNode *node =
        tt_new(low->tt_arena, TT_VARIABLE_DECLARATION, &TYPE_UNIT_INSTANCE, ast->location);
    node->variable_declaration.symbol = symbol;
    node->variable_declaration.name = name;
    node->variable_declaration.var_type = type;
    node->variable_declaration.initializer = init;
    node->variable_declaration.is_mut = is_mut;
    return node;
}

static TtNode *lower_assign(Lowering *low, const ASTNode *ast) {
    TtNode *target = lower_expression(low, ast->assign.target);
    TtNode *value = lower_expression(low, ast->assign.value);
    TtNode *node = tt_new(low->tt_arena, TT_ASSIGN, &TYPE_UNIT_INSTANCE, ast->location);
    node->assign.target = target;
    node->assign.value = value;
    return node;
}

/** Desugar `x op= expr` → `x = x op expr`. */
static TtNode *lower_compound_assign(Lowering *low, const ASTNode *ast) {
    TtNode *target = lower_expression(low, ast->compound_assign.target);
    TtNode *value = lower_expression(low, ast->compound_assign.value);

    // Build binary: target op value
    TokenKind base_op = lowering_compound_to_base_op(ast->compound_assign.op);
    TtNode *target_read = lower_expression(low, ast->compound_assign.target);
    TtNode *binary = tt_new(low->tt_arena, TT_BINARY, target->type, ast->location);
    binary->binary.op = base_op;
    binary->binary.left = target_read;
    binary->binary.right = value;

    // Assign: target = binary
    TtNode *node = tt_new(low->tt_arena, TT_ASSIGN, &TYPE_UNIT_INSTANCE, ast->location);
    node->assign.target = target;
    node->assign.value = binary;
    return node;
}

TtNode *lower_block(Lowering *low, const ASTNode *ast) {
    if (ast == NULL) {
        return NULL;
    }
    assert(ast->kind == NODE_BLOCK);
    lowering_scope_enter(low);

    TtNode **statements = NULL;
    for (int32_t i = 0; i < BUFFER_LENGTH(ast->block.statements); i++) {
        TtNode *stmt = lower_node(low, ast->block.statements[i]);
        if (stmt != NULL) {
            BUFFER_PUSH(statements, stmt);
        }
    }

    TtNode *result = NULL;
    if (ast->block.result != NULL) {
        result = lower_expression(low, ast->block.result);
    }

    lowering_scope_leave(low);

    TtNode *node = tt_new(low->tt_arena, TT_BLOCK,
                          ast->type != NULL ? ast->type : &TYPE_UNIT_INSTANCE, ast->location);
    node->block.statements = statements;
    node->block.result = result;
    return node;
}

static TtNode *lower_loop(Lowering *low, const ASTNode *ast) {
    TtNode *body = lower_block(low, ast->loop.body);
    TtNode *node = tt_new(low->tt_arena, TT_LOOP, &TYPE_UNIT_INSTANCE, ast->location);
    node->loop.body = body;
    return node;
}

/**
 * Desugar `for var := start..end { body }` into:
 * @code
 *     {
 *         var _end: i32 = end
 *         var var: i32 = start
 *         loop {
 *             if var >= _end { break }
 *             body_statements...
 *             var = var + 1
 *         }
 *     }
 * @endcode
 */
static TtNode *lower_for(Lowering *low, const ASTNode *ast) {
    SourceLocation loc = ast->location;
    const Type *iter_type = &TYPE_I32_INSTANCE;

    lowering_scope_enter(low);

    // var _end = end
    const char *end_name = lowering_make_temp_name(low);
    TtSymbol *end_sym =
        lowering_make_symbol(low, TT_SYMBOL_VARIABLE, end_name, iter_type, false, loc);
    lowering_scope_add(low, end_name, end_sym);
    TtNode *end_init = lower_expression(low, ast->for_loop.end);
    TtNode *end_decl = tt_new(low->tt_arena, TT_VARIABLE_DECLARATION, &TYPE_UNIT_INSTANCE, loc);
    end_decl->variable_declaration.symbol = end_sym;
    end_decl->variable_declaration.name = end_name;
    end_decl->variable_declaration.var_type = iter_type;
    end_decl->variable_declaration.initializer = end_init;
    end_decl->variable_declaration.is_mut = false;

    // var i = start
    const char *var_name = ast->for_loop.variable_name;
    TtSymbol *var_sym =
        lowering_make_symbol(low, TT_SYMBOL_VARIABLE, var_name, iter_type, true, loc);
    lowering_scope_add(low, var_name, var_sym);
    TtNode *start_init = lower_expression(low, ast->for_loop.start);
    TtNode *var_decl = tt_new(low->tt_arena, TT_VARIABLE_DECLARATION, &TYPE_UNIT_INSTANCE, loc);
    var_decl->variable_declaration.symbol = var_sym;
    var_decl->variable_declaration.name = var_name;
    var_decl->variable_declaration.var_type = iter_type;
    var_decl->variable_declaration.initializer = start_init;
    var_decl->variable_declaration.is_mut = true;

    // Loop body statements
    TtNode **loop_stmts = NULL;

    // if var >= _end { break }
    TtNode *cmp = tt_new(low->tt_arena, TT_BINARY, &TYPE_BOOL_INSTANCE, loc);
    cmp->binary.op = TOKEN_GREATER_EQUAL;
    cmp->binary.left = lowering_make_var_ref(low, var_sym, loc);
    cmp->binary.right = lowering_make_var_ref(low, end_sym, loc);

    TtNode *break_node = tt_new(low->tt_arena, TT_BREAK, &TYPE_UNIT_INSTANCE, loc);

    TtNode **break_stmts = NULL;
    BUFFER_PUSH(break_stmts, break_node);
    TtNode *break_block = tt_new(low->tt_arena, TT_BLOCK, &TYPE_UNIT_INSTANCE, loc);
    break_block->block.statements = break_stmts;

    TtNode *if_break = tt_new(low->tt_arena, TT_IF, &TYPE_UNIT_INSTANCE, loc);
    if_break->if_expression.condition = cmp;
    if_break->if_expression.then_body = break_block;

    BUFFER_PUSH(loop_stmts, if_break);

    // body statements (inline the block's statements, not wrapping in another block)
    if (ast->for_loop.body != NULL && ast->for_loop.body->kind == NODE_BLOCK) {
        for (int32_t i = 0; i < BUFFER_LENGTH(ast->for_loop.body->block.statements); i++) {
            TtNode *stmt = lower_node(low, ast->for_loop.body->block.statements[i]);
            if (stmt != NULL) {
                BUFFER_PUSH(loop_stmts, stmt);
            }
        }
    }

    // var = var + 1
    TtNode *increment = tt_new(low->tt_arena, TT_BINARY, iter_type, loc);
    increment->binary.op = TOKEN_PLUS;
    increment->binary.left = lowering_make_var_ref(low, var_sym, loc);
    increment->binary.right = lowering_make_int_lit(low, 1, iter_type, TYPE_I32, loc);

    TtNode *inc_assign = tt_new(low->tt_arena, TT_ASSIGN, &TYPE_UNIT_INSTANCE, loc);
    inc_assign->assign.target = lowering_make_var_ref(low, var_sym, loc);
    inc_assign->assign.value = increment;
    BUFFER_PUSH(loop_stmts, inc_assign);

    // Build loop block
    TtNode *loop_block = tt_new(low->tt_arena, TT_BLOCK, &TYPE_UNIT_INSTANCE, loc);
    loop_block->block.statements = loop_stmts;

    TtNode *loop_node = tt_new(low->tt_arena, TT_LOOP, &TYPE_UNIT_INSTANCE, loc);
    loop_node->loop.body = loop_block;

    // Build outer block: { end_decl; var_decl; loop { ... } }
    TtNode **outer_stmts = NULL;
    BUFFER_PUSH(outer_stmts, end_decl);
    BUFFER_PUSH(outer_stmts, var_decl);
    BUFFER_PUSH(outer_stmts, loop_node);

    lowering_scope_leave(low);

    TtNode *outer = tt_new(low->tt_arena, TT_BLOCK, &TYPE_UNIT_INSTANCE, loc);
    outer->block.statements = outer_stmts;
    return outer;
}

static TtNode *lower_expression_statement(Lowering *low, const ASTNode *ast) {
    TtNode *expr = lower_expression(low, ast->expression_statement.expression);
    TtNode *node =
        tt_new(low->tt_arena, TT_EXPRESSION_STATEMENT, &TYPE_UNIT_INSTANCE, ast->location);
    node->expression_statement.expression = expr;
    return node;
}

TtNode *lower_node(Lowering *low, const ASTNode *ast) {
    if (ast == NULL) {
        return NULL;
    }

    switch (ast->kind) {
    case NODE_FILE: {
        lowering_scope_enter(low);

        // Pre-register all functions in scope before lowering bodies
        for (int32_t i = 0; i < BUFFER_LENGTH(ast->file.declarations); i++) {
            const ASTNode *decl = ast->file.declarations[i];
            if (decl->kind == NODE_FUNCTION_DECLARATION) {
                const Type *ret = decl->type != NULL ? decl->type : &TYPE_UNIT_INSTANCE;
                TtSymbol *sym =
                    lowering_make_symbol(low, TT_SYMBOL_FUNCTION, decl->function_declaration.name,
                                         ret, false, decl->location);
                lowering_scope_add(low, decl->function_declaration.name, sym);
            }
        }

        TtNode **declarations = NULL;
        for (int32_t i = 0; i < BUFFER_LENGTH(ast->file.declarations); i++) {
            TtNode *decl = lower_node(low, ast->file.declarations[i]);
            if (decl != NULL) {
                BUFFER_PUSH(declarations, decl);
            }
        }
        lowering_scope_leave(low);

        TtNode *file_node = tt_new(low->tt_arena, TT_FILE, &TYPE_UNIT_INSTANCE, ast->location);
        file_node->file.declarations = declarations;
        return file_node;
    }

    case NODE_MODULE: {
        low->current_module = ast->module.name;
        TtNode *node = tt_new(low->tt_arena, TT_MODULE, &TYPE_UNIT_INSTANCE, ast->location);
        node->module.name = ast->module.name;
        return node;
    }

    case NODE_TYPE_ALIAS: {
        TtNode *node = tt_new(low->tt_arena, TT_TYPE_ALIAS, &TYPE_UNIT_INSTANCE, ast->location);
        node->type_alias.name = ast->type_alias.name;
        node->type_alias.is_public = false;
        node->type_alias.underlying = ast->type;
        return node;
    }

    case NODE_FUNCTION_DECLARATION:
        return lower_function_declaration(low, ast);

    case NODE_VARIABLE_DECLARATION:
        return lower_variable_declaration(low, ast);

    case NODE_EXPRESSION_STATEMENT:
        return lower_expression_statement(low, ast);

    case NODE_BREAK: {
        TtNode *node = tt_new(low->tt_arena, TT_BREAK, &TYPE_UNIT_INSTANCE, ast->location);
        return node;
    }

    case NODE_CONTINUE:
        return tt_new(low->tt_arena, TT_CONTINUE, &TYPE_UNIT_INSTANCE, ast->location);

    case NODE_ASSIGN:
        return lower_assign(low, ast);

    case NODE_COMPOUND_ASSIGN:
        return lower_compound_assign(low, ast);

    case NODE_LOOP:
        return lower_loop(low, ast);

    case NODE_FOR:
        return lower_for(low, ast);

    case NODE_BLOCK:
        return lower_block(low, ast);

    case NODE_IF:
        return lower_statement_if(low, ast);

    default:
        // Expressions at statement level
        return lower_expression(low, ast);
    }
}
