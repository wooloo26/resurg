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

// ── Public API ─────────────────────────────────────────────────────────

Lowering *lowering_create(Arena *tt_arena, Arena *sema_arena) {
    Lowering *low = rsg_malloc(sizeof(Lowering));
    low->tt_arena = tt_arena;
    low->sema_arena = sema_arena;
    low->scope = NULL;
    low->error_count = 0;
    low->current_module = NULL;
    low->temp_counter = 0;
    low->shadow_counter = 0;
    low->compound_types = NULL;
    return low;
}

void lowering_destroy(Lowering *lowering) {
    BUFFER_FREE(lowering->compound_types);
    free(lowering);
}

// ── Compound type collection ──────────────────────────────────────────

/** Register an array or tuple type (and its children) in the lowering registry. */
static void register_compound_type(Lowering *low, const Type *type) {
    if (type == NULL) {
        return;
    }
    if (type->kind == TYPE_ARRAY) {
        register_compound_type(low, type->array.element);
        for (int32_t i = 0; i < BUFFER_LENGTH(low->compound_types); i++) {
            if (type_equal(low->compound_types[i], type)) {
                return;
            }
        }
        BUFFER_PUSH(low->compound_types, type);
    } else if (type->kind == TYPE_TUPLE) {
        for (int32_t i = 0; i < type->tuple.count; i++) {
            register_compound_type(low, type->tuple.elements[i]);
        }
        for (int32_t i = 0; i < BUFFER_LENGTH(low->compound_types); i++) {
            if (type_equal(low->compound_types[i], type)) {
                return;
            }
        }
        BUFFER_PUSH(low->compound_types, type);
    }
}

static void collect_compound_types(Lowering *low, const TtNode *node);

static void collect_children(Lowering *low, TtNode **children, int32_t count) {
    for (int32_t i = 0; i < count; i++) {
        collect_compound_types(low, children[i]);
    }
}

/** Walk @p node collecting all array/tuple types into the lowering registry. */
static void collect_compound_types(Lowering *low, const TtNode *node) {
    if (node == NULL) {
        return;
    }
    if (node->type != NULL) {
        register_compound_type(low, node->type);
    }
    switch (node->kind) {
    case TT_FILE:
        collect_children(low, node->file.declarations, BUFFER_LENGTH(node->file.declarations));
        break;
    case TT_FUNCTION_DECLARATION:
        collect_children(low, node->function_declaration.params,
                         BUFFER_LENGTH(node->function_declaration.params));
        collect_compound_types(low, node->function_declaration.body);
        break;
    case TT_BLOCK:
        collect_children(low, node->block.statements, BUFFER_LENGTH(node->block.statements));
        collect_compound_types(low, node->block.result);
        break;
    case TT_VARIABLE_DECLARATION:
        collect_compound_types(low, node->variable_declaration.initializer);
        break;
    case TT_RETURN:
        collect_compound_types(low, node->return_statement.value);
        break;
    case TT_BINARY:
        collect_compound_types(low, node->binary.left);
        collect_compound_types(low, node->binary.right);
        break;
    case TT_UNARY:
        collect_compound_types(low, node->unary.operand);
        break;
    case TT_CALL:
        collect_compound_types(low, node->call.callee);
        collect_children(low, node->call.arguments, BUFFER_LENGTH(node->call.arguments));
        break;
    case TT_IF:
        collect_compound_types(low, node->if_expression.condition);
        collect_compound_types(low, node->if_expression.then_body);
        collect_compound_types(low, node->if_expression.else_body);
        break;
    case TT_ASSIGN:
        collect_compound_types(low, node->assign.target);
        collect_compound_types(low, node->assign.value);
        break;
    case TT_ARRAY_LITERAL:
        collect_children(low, node->array_literal.elements,
                         BUFFER_LENGTH(node->array_literal.elements));
        break;
    case TT_TUPLE_LITERAL:
        collect_children(low, node->tuple_literal.elements,
                         BUFFER_LENGTH(node->tuple_literal.elements));
        break;
    case TT_INDEX:
        collect_compound_types(low, node->index_access.object);
        collect_compound_types(low, node->index_access.index);
        break;
    case TT_TUPLE_INDEX:
        collect_compound_types(low, node->tuple_index.object);
        break;
    case TT_TYPE_CONVERSION:
        collect_compound_types(low, node->type_conversion.operand);
        break;
    case TT_MODULE_ACCESS:
        collect_compound_types(low, node->module_access.object);
        break;
    case TT_LOOP:
        collect_compound_types(low, node->loop.body);
        break;
    default:
        break;
    }
}

TtNode *lowering_lower(Lowering *lowering, const ASTNode *file) {
    TtNode *tt_file = lower_node(lowering, file);
    collect_compound_types(lowering, tt_file);
    tt_file->file.compound_types = lowering->compound_types;
    return tt_file;
}
