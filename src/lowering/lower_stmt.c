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

    return lowering_make_var_decl(low, symbol, name, type, init, is_mut, ast->location);
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

/** Expand a struct destructure into variable declarations in @p stmts. */
static void lower_struct_destructure_into(Lowering *low, const ASTNode *ast, TtNode ***stmts) {
    TtNode *value = lower_expression(low, ast->struct_destructure.value);
    const Type *struct_type = value->type;

    const char *tmp_name = lowering_make_temp_name(low);
    TtSymbol *tmp_sym =
        lowering_make_symbol(low, TT_SYMBOL_VARIABLE, tmp_name, struct_type, false, ast->location);
    lowering_scope_add(low, tmp_name, tmp_sym);
    BUFFER_PUSH(*stmts, lowering_make_var_decl(low, tmp_sym, tmp_name, struct_type, value, false,
                                               ast->location));

    for (int32_t i = 0; i < BUFFER_LENGTH(ast->struct_destructure.field_names); i++) {
        const char *fname = ast->struct_destructure.field_names[i];
        const char *alias = (ast->struct_destructure.aliases != NULL &&
                             i < BUFFER_LENGTH(ast->struct_destructure.aliases))
                                ? ast->struct_destructure.aliases[i]
                                : NULL;
        const char *var_name = (alias != NULL) ? alias : fname;
        const Type *field_type = NULL;
        TtNode *field_access = NULL;

        // Check direct fields
        const StructField *sf = type_struct_find_field(struct_type, fname);
        if (sf != NULL) {
            field_type = sf->type;
            field_access = tt_new(low->tt_arena, TT_STRUCT_FIELD_ACCESS, field_type, ast->location);
            field_access->struct_field_access.object =
                lowering_make_var_ref(low, tmp_sym, ast->location);
            field_access->struct_field_access.field = fname;
            field_access->struct_field_access.via_pointer = false;
        } else {
            // Check promoted fields
            for (int32_t ei = 0; ei < struct_type->struct_type.embed_count; ei++) {
                const Type *et = struct_type->struct_type.embedded[ei];
                sf = type_struct_find_field(et, fname);
                if (sf != NULL) {
                    field_type = sf->type;
                    TtNode *embed =
                        tt_new(low->tt_arena, TT_STRUCT_FIELD_ACCESS, et, ast->location);
                    embed->struct_field_access.object =
                        lowering_make_var_ref(low, tmp_sym, ast->location);
                    embed->struct_field_access.field = et->struct_type.name;
                    embed->struct_field_access.via_pointer = false;

                    field_access =
                        tt_new(low->tt_arena, TT_STRUCT_FIELD_ACCESS, field_type, ast->location);
                    field_access->struct_field_access.object = embed;
                    field_access->struct_field_access.field = fname;
                    field_access->struct_field_access.via_pointer = false;
                    break;
                }
            }
        }

        if (field_type == NULL) {
            field_type = &TYPE_ERROR_INSTANCE;
        }

        TtSymbol *var_sym = lowering_make_symbol(low, TT_SYMBOL_VARIABLE, var_name, field_type,
                                                 false, ast->location);
        lowering_scope_add(low, var_name, var_sym);
        BUFFER_PUSH(*stmts, lowering_make_var_decl(low, var_sym, var_name, field_type, field_access,
                                                   false, ast->location));
    }
}

/** Expand a tuple destructure into variable declarations in @p stmts. */
static void lower_tuple_destructure_into(Lowering *low, const ASTNode *ast, TtNode ***stmts) {
    TtNode *value = lower_expression(low, ast->tuple_destructure.value);
    const Type *tuple_type = value->type;

    const char *tmp_name = lowering_make_temp_name(low);
    TtSymbol *tmp_sym =
        lowering_make_symbol(low, TT_SYMBOL_VARIABLE, tmp_name, tuple_type, false, ast->location);
    lowering_scope_add(low, tmp_name, tmp_sym);
    BUFFER_PUSH(*stmts, lowering_make_var_decl(low, tmp_sym, tmp_name, tuple_type, value, false,
                                               ast->location));

    int32_t name_count = BUFFER_LENGTH(ast->tuple_destructure.names);
    bool has_rest = ast->tuple_destructure.has_rest;
    int32_t rest_pos = ast->tuple_destructure.rest_position;
    int32_t tuple_count =
        (tuple_type != NULL && tuple_type->kind == TYPE_TUPLE) ? tuple_type->tuple.count : 0;
    int32_t skipped = has_rest ? (tuple_count - name_count) : 0;

    for (int32_t i = 0; i < name_count; i++) {
        const char *vname = ast->tuple_destructure.names[i];

        // Compute the element index, accounting for `..`
        int32_t elem_idx = i;
        if (has_rest && i >= rest_pos) {
            elem_idx = i + skipped;
        }

        const Type *elem_type = &TYPE_ERROR_INSTANCE;
        if (tuple_type != NULL && tuple_type->kind == TYPE_TUPLE && elem_idx < tuple_count) {
            elem_type = tuple_type->tuple.elements[elem_idx];
        }

        // Skip variable creation for `_` or `_`-prefixed names
        if (vname[0] == '_') {
            continue;
        }

        TtNode *idx_access = tt_new(low->tt_arena, TT_TUPLE_INDEX, elem_type, ast->location);
        idx_access->tuple_index.object = lowering_make_var_ref(low, tmp_sym, ast->location);
        idx_access->tuple_index.element_index = elem_idx;

        TtSymbol *var_sym =
            lowering_make_symbol(low, TT_SYMBOL_VARIABLE, vname, elem_type, false, ast->location);
        lowering_scope_add(low, vname, var_sym);
        BUFFER_PUSH(*stmts, lowering_make_var_decl(low, var_sym, vname, elem_type, idx_access,
                                                   false, ast->location));
    }
}

TtNode *lower_block(Lowering *low, const ASTNode *ast) {
    if (ast == NULL) {
        return NULL;
    }
    assert(ast->kind == NODE_BLOCK);
    lowering_scope_enter(low);

    TtNode **statements = NULL;
    for (int32_t i = 0; i < BUFFER_LENGTH(ast->block.statements); i++) {
        const ASTNode *stmt = ast->block.statements[i];
        if (stmt->kind == NODE_STRUCT_DESTRUCTURE) {
            lower_struct_destructure_into(low, stmt, &statements);
        } else if (stmt->kind == NODE_TUPLE_DESTRUCTURE) {
            lower_tuple_destructure_into(low, stmt, &statements);
        } else {
            TtNode *s = lower_node(low, stmt);
            if (s != NULL) {
                BUFFER_PUSH(statements, s);
            }
        }
    }

    TtNode *result = NULL;
    if (ast->block.result != NULL) {
        // Assignments in result position are side-effect-only statements.
        if (ast->block.result->kind == NODE_ASSIGN) {
            TtNode *stmt = lower_assign(low, ast->block.result);
            BUFFER_PUSH(statements, stmt);
        } else if (ast->block.result->kind == NODE_COMPOUND_ASSIGN) {
            TtNode *stmt = lower_compound_assign(low, ast->block.result);
            BUFFER_PUSH(statements, stmt);
        } else {
            result = lower_expression(low, ast->block.result);
        }
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

/** Build `if iter >= end_bound { break }` guard for desugared for-loop. */
static TtNode *build_for_guard(Lowering *low, TtSymbol *iter_sym, TtSymbol *end_sym,
                               SourceLocation loc) {
    TtNode *cond = tt_new(low->tt_arena, TT_BINARY, &TYPE_BOOL_INSTANCE, loc);
    cond->binary.op = TOKEN_GREATER_EQUAL;
    cond->binary.left = lowering_make_var_ref(low, iter_sym, loc);
    cond->binary.right = lowering_make_var_ref(low, end_sym, loc);

    TtNode *break_node = tt_new(low->tt_arena, TT_BREAK, &TYPE_UNIT_INSTANCE, loc);
    TtNode **break_stmts = NULL;
    BUFFER_PUSH(break_stmts, break_node);
    TtNode *break_block = tt_new(low->tt_arena, TT_BLOCK, &TYPE_UNIT_INSTANCE, loc);
    break_block->block.statements = break_stmts;
    break_block->block.result = NULL;

    TtNode *guard = tt_new(low->tt_arena, TT_IF, &TYPE_UNIT_INSTANCE, loc);
    guard->if_expression.condition = cond;
    guard->if_expression.then_body = break_block;
    guard->if_expression.else_body = NULL;
    return guard;
}

/** Build `iter = iter + 1` increment for desugared for-loop. */
static TtNode *build_for_increment(Lowering *low, TtSymbol *iter_sym, const Type *iter_type,
                                   SourceLocation loc) {
    TtNode *increment = tt_new(low->tt_arena, TT_BINARY, iter_type, loc);
    increment->binary.op = TOKEN_PLUS;
    increment->binary.left = lowering_make_var_ref(low, iter_sym, loc);
    increment->binary.right = lowering_make_int_lit(low, 1, iter_type, TYPE_I32, loc);

    TtNode *assign = tt_new(low->tt_arena, TT_ASSIGN, &TYPE_UNIT_INSTANCE, loc);
    assign->assign.target = lowering_make_var_ref(low, iter_sym, loc);
    assign->assign.value = increment;
    return assign;
}

/**
 * Rewrite TT_CONTINUE nodes inside a for-loop body to include the
 * iterator increment before the continue.  Skips nested TT_LOOP nodes
 * (their own continue semantics are independent).
 *
 * Transforms: `continue` → `{ i = i + 1; continue; }`
 */
static void rewrite_continue_for_increment(Lowering *low, TtNode **node_ptr, TtSymbol *iter_sym,
                                           const Type *iter_type, SourceLocation loc) {
    TtNode *node = *node_ptr;
    if (node == NULL) {
        return;
    }

    if (node->kind == TT_CONTINUE) {
        // Build: { i = i + 1; continue; }
        TtNode *assign = build_for_increment(low, iter_sym, iter_type, loc);

        TtNode **stmts = NULL;
        BUFFER_PUSH(stmts, assign);
        BUFFER_PUSH(stmts, node); // the original TT_CONTINUE

        TtNode *block = tt_new(low->tt_arena, TT_BLOCK, &TYPE_UNIT_INSTANCE, node->location);
        block->block.statements = stmts;
        block->block.result = NULL;
        *node_ptr = block;
        return;
    }

    // Do not descend into nested loops — their continue is independent
    if (node->kind == TT_LOOP) {
        return;
    }

    // Recurse into children
    switch (node->kind) {
    case TT_BLOCK:
        for (int32_t i = 0; i < BUFFER_LENGTH(node->block.statements); i++) {
            rewrite_continue_for_increment(low, &node->block.statements[i], iter_sym, iter_type,
                                           loc);
        }
        if (node->block.result != NULL) {
            rewrite_continue_for_increment(low, &node->block.result, iter_sym, iter_type, loc);
        }
        break;
    case TT_IF:
        rewrite_continue_for_increment(low, &node->if_expression.then_body, iter_sym, iter_type,
                                       loc);
        rewrite_continue_for_increment(low, &node->if_expression.else_body, iter_sym, iter_type,
                                       loc);
        break;
    default:
        break;
    }
}

/**
 * Lower the user's for-loop body, rewrite continue nodes, and append the
 * resulting statements to @p out_stmts.
 */
static void build_for_user_body(Lowering *low, const ASTNode *body_ast, TtSymbol *var_sym,
                                const Type *iter_type, SourceLocation loc, TtNode ***out_stmts) {
    if (body_ast == NULL || body_ast->kind != NODE_BLOCK) {
        return;
    }
    TtNode *user_body = lower_block(low, body_ast);
    if (user_body == NULL || user_body->kind != TT_BLOCK) {
        return;
    }
    rewrite_continue_for_increment(low, &user_body, var_sym, iter_type, loc);
    for (int32_t i = 0; i < BUFFER_LENGTH(user_body->block.statements); i++) {
        BUFFER_PUSH(*out_stmts, user_body->block.statements[i]);
    }
    if (user_body->block.result != NULL) {
        BUFFER_PUSH(*out_stmts, user_body->block.result);
    }
}

/**
 * Lower `for var := start..end { body }` into desugared TT_LOOP.
 *
 * Desugaring:
 *   { var _end = end; var i = start; loop { if i >= _end { break } body; i = i + 1 } }
 */
static TtNode *lower_for(Lowering *low, const ASTNode *ast) {
    SourceLocation loc = ast->location;
    const Type *iter_type = &TYPE_I32_INSTANCE;

    lowering_scope_enter(low);

    // var _end = end
    TtNode *end_expr = lower_expression(low, ast->for_loop.end);
    const char *end_name = lowering_make_temp_name(low);
    TtSymbol *end_sym =
        lowering_make_symbol(low, TT_SYMBOL_VARIABLE, end_name, iter_type, false, loc);
    lowering_scope_add(low, end_name, end_sym);
    TtNode *end_decl =
        lowering_make_var_decl(low, end_sym, end_name, iter_type, end_expr, false, loc);

    // var i = start
    const char *var_name = ast->for_loop.variable_name;
    TtNode *start = lower_expression(low, ast->for_loop.start);
    TtSymbol *var_sym =
        lowering_make_symbol(low, TT_SYMBOL_VARIABLE, var_name, iter_type, true, loc);
    lowering_scope_add(low, var_name, var_sym);
    TtNode *iter_decl = lowering_make_var_decl(low, var_sym, var_name, iter_type, start, true, loc);

    // Build loop body: guard + user body + increment
    TtNode **loop_stmts = NULL;
    BUFFER_PUSH(loop_stmts, build_for_guard(low, var_sym, end_sym, loc));
    build_for_user_body(low, ast->for_loop.body, var_sym, iter_type, loc, &loop_stmts);
    BUFFER_PUSH(loop_stmts, build_for_increment(low, var_sym, iter_type, loc));

    TtNode *loop_body = tt_new(low->tt_arena, TT_BLOCK, &TYPE_UNIT_INSTANCE, loc);
    loop_body->block.statements = loop_stmts;
    loop_body->block.result = NULL;

    TtNode *loop_node = tt_new(low->tt_arena, TT_LOOP, &TYPE_UNIT_INSTANCE, loc);
    loop_node->loop.body = loop_body;

    // Outer block: { var _end = ...; var i = ...; loop { ... } }
    TtNode **outer_stmts = NULL;
    BUFFER_PUSH(outer_stmts, end_decl);
    BUFFER_PUSH(outer_stmts, iter_decl);
    BUFFER_PUSH(outer_stmts, loop_node);

    lowering_scope_leave(low);

    TtNode *outer = tt_new(low->tt_arena, TT_BLOCK, &TYPE_UNIT_INSTANCE, loc);
    outer->block.statements = outer_stmts;
    outer->block.result = NULL;
    return outer;
}

static TtNode *lower_expression_statement(Lowering *low, const ASTNode *ast) {
    const ASTNode *inner = ast->expression_statement.expression;

    // Assignments are statements in TT — unwrap and lower directly.
    if (inner->kind == NODE_ASSIGN) {
        return lower_assign(low, inner);
    }
    if (inner->kind == NODE_COMPOUND_ASSIGN) {
        return lower_compound_assign(low, inner);
    }

    return lower_expression(low, inner);
}

/** Pre-register all function declarations into scope before lowering bodies. */
static void preregister_functions(Lowering *low, const ASTNode *file_ast) {
    for (int32_t i = 0; i < BUFFER_LENGTH(file_ast->file.declarations); i++) {
        const ASTNode *decl = file_ast->file.declarations[i];
        if (decl->kind == NODE_FUNCTION_DECLARATION) {
            const Type *ret = decl->type != NULL ? decl->type : &TYPE_UNIT_INSTANCE;
            TtSymbol *sym =
                lowering_make_symbol(low, TT_SYMBOL_FUNCTION, decl->function_declaration.name, ret,
                                     false, decl->location);
            sym->mangled_name =
                arena_sprintf(low->tt_arena, "rsgu_%s", decl->function_declaration.name);
            lowering_scope_add(low, decl->function_declaration.name, sym);
        }
        if (decl->kind == NODE_STRUCT_DECLARATION) {
            const char *struct_name = decl->struct_declaration.name;
            for (int32_t j = 0; j < BUFFER_LENGTH(decl->struct_declaration.methods); j++) {
                const ASTNode *method = decl->struct_declaration.methods[j];
                const char *method_name = method->function_declaration.name;
                const Type *ret = method->type != NULL ? method->type : &TYPE_UNIT_INSTANCE;
                const char *key = arena_sprintf(low->tt_arena, "%s.%s", struct_name, method_name);
                const char *mangled =
                    arena_sprintf(low->tt_arena, "rsgu_%s_%s", struct_name, method_name);
                TtSymbol *sym = lowering_make_symbol(low, TT_SYMBOL_FUNCTION, key, ret, false,
                                                     method->location);
                sym->mangled_name = mangled;
                lowering_scope_add(low, key, sym);
            }
        }
    }
}

/** Lower a NODE_FILE into a TT_FILE with pre-registered function symbols. */
static TtNode *lower_file(Lowering *low, const ASTNode *ast) {
    lowering_scope_enter(low);
    preregister_functions(low, ast);

    TtNode **declarations = NULL;
    for (int32_t i = 0; i < BUFFER_LENGTH(ast->file.declarations); i++) {
        const ASTNode *decl_ast = ast->file.declarations[i];

        if (decl_ast->kind == NODE_STRUCT_DECLARATION) {
            // Emit struct type declaration
            TtNode *struct_decl = tt_new(low->tt_arena, TT_STRUCT_DECLARATION, &TYPE_UNIT_INSTANCE,
                                         decl_ast->location);
            struct_decl->struct_decl.name = decl_ast->struct_declaration.name;
            struct_decl->struct_decl.struct_type = decl_ast->type;
            BUFFER_PUSH(declarations, struct_decl);

            // Emit each method as a function declaration
            for (int32_t j = 0; j < BUFFER_LENGTH(decl_ast->struct_declaration.methods); j++) {
                TtNode *method =
                    lower_method_declaration(low, decl_ast->struct_declaration.methods[j],
                                             decl_ast->struct_declaration.name, decl_ast->type);
                if (method != NULL) {
                    BUFFER_PUSH(declarations, method);
                }
            }
            continue;
        }

        TtNode *decl = lower_node(low, decl_ast);
        if (decl != NULL) {
            BUFFER_PUSH(declarations, decl);
        }
    }
    lowering_scope_leave(low);

    TtNode *file_node = tt_new(low->tt_arena, TT_FILE, &TYPE_UNIT_INSTANCE, ast->location);
    file_node->file.declarations = declarations;
    return file_node;
}

TtNode *lower_node(Lowering *low, const ASTNode *ast) {
    if (ast == NULL) {
        return NULL;
    }

    switch (ast->kind) {
    case NODE_FILE:
        return lower_file(low, ast);

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

    case NODE_STRUCT_DECLARATION: {
        // Handled primarily in lower_file; fallback for other contexts
        TtNode *node =
            tt_new(low->tt_arena, TT_STRUCT_DECLARATION, &TYPE_UNIT_INSTANCE, ast->location);
        node->struct_decl.name = ast->struct_declaration.name;
        node->struct_decl.struct_type = ast->type;
        return node;
    }

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
