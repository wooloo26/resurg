#include "lowering.h"

#include "ast.h"
#include "sema/_sema.h"

// ── Lowering scope ─────────────────────────────────────────────────────

typedef struct LoweringScope LoweringScope;

struct LoweringScope {
    HashTable table;
    LoweringScope *parent;
};

struct Lowering {
    Arena *tt_arena;
    Arena *sema_arena;
    LoweringScope *scope;
    int32_t error_count;
    const char *current_module;
    int32_t temp_counter;
};

static void scope_enter(Lowering *low) {
    LoweringScope *scope = arena_alloc(low->tt_arena, sizeof(LoweringScope));
    hash_table_init(&scope->table, low->tt_arena);
    scope->parent = low->scope;
    low->scope = scope;
}

static void scope_leave(Lowering *low) {
    low->scope = low->scope->parent;
}

static void scope_add(Lowering *low, const char *name, TtSymbol *symbol) {
    hash_table_insert(&low->scope->table, name, symbol);
}

static TtSymbol *scope_find(const Lowering *low, const char *name) {
    for (LoweringScope *scope = low->scope; scope != NULL; scope = scope->parent) {
        TtSymbol *symbol = hash_table_lookup(&scope->table, name);
        if (symbol != NULL) {
            return symbol;
        }
    }
    return NULL;
}

// ── Helpers ────────────────────────────────────────────────────────────

/** Create a Sema Symbol and wrap it in a TtSymbol. */
static TtSymbol *make_symbol(Lowering *low, TtSymbolKind kind, const char *name, const Type *type,
                             bool is_mut, SourceLocation location) {
    Symbol *sema_sym = arena_alloc(low->sema_arena, sizeof(Symbol));
    memset(sema_sym, 0, sizeof(Symbol));
    sema_sym->name = name;
    sema_sym->type = type;
    switch (kind) {
    case TT_SYM_VAR:
        sema_sym->kind = SYM_VAR;
        break;
    case TT_SYM_PARAM:
        sema_sym->kind = SYM_PARAM;
        break;
    case TT_SYM_FUNCTION:
        sema_sym->kind = SYM_FUNCTION;
        break;
    case TT_SYM_TYPE:
        sema_sym->kind = SYM_TYPE;
        break;
    case TT_SYM_MODULE:
        sema_sym->kind = SYM_MODULE;
        break;
    }
    return tt_symbol_new(low->tt_arena, kind, sema_sym, is_mut, location);
}

/** Generate a unique temporary name like _tt_tmp_0. */
static const char *make_temp_name(Lowering *low) {
    return arena_sprintf(low->tt_arena, "_tt_tmp_%d", low->temp_counter++);
}

/** Create a TtVarRef node. */
static TtNode *make_var_ref(Lowering *low, TtSymbol *symbol, SourceLocation location) {
    TtNode *node = tt_new(low->tt_arena, TT_VAR_REF, tt_symbol_type(symbol), location);
    node->var_ref.symbol = symbol;
    return node;
}

/** Create a TtIntLit node. */
static TtNode *make_int_lit(Lowering *low, uint64_t value, const Type *type, TypeKind int_kind,
                            SourceLocation location) {
    TtNode *node = tt_new(low->tt_arena, TT_INT_LIT, type, location);
    node->int_lit.value = value;
    node->int_lit.int_kind = int_kind;
    return node;
}

/** Map a compound-assignment TokenKind to its base arithmetic operator. */
static TokenKind compound_to_base_op(TokenKind op) {
    switch (op) {
    case TOKEN_PLUS_EQUAL:
        return TOKEN_PLUS;
    case TOKEN_MINUS_EQUAL:
        return TOKEN_MINUS;
    case TOKEN_STAR_EQUAL:
        return TOKEN_STAR;
    case TOKEN_SLASH_EQUAL:
        return TOKEN_SLASH;
    default:
        return op;
    }
}

// ── Forward declarations ───────────────────────────────────────────────

static TtNode *lower_node(Lowering *low, const ASTNode *ast);
static TtNode *lower_expression(Lowering *low, const ASTNode *ast);
static TtNode *lower_block(Lowering *low, const ASTNode *ast);

// ── Expression lowering ───────────────────────────────────────────────

static TtNode *lower_literal(Lowering *low, const ASTNode *ast) {
    SourceLocation loc = ast->location;
    const Type *type = ast->type;

    switch (ast->literal.kind) {
    case LITERAL_BOOL: {
        TtNode *node = tt_new(low->tt_arena, TT_BOOL_LIT, type, loc);
        node->bool_lit.value = ast->literal.boolean_value;
        return node;
    }
    case LITERAL_I8:
    case LITERAL_I16:
    case LITERAL_I32:
    case LITERAL_I64:
    case LITERAL_I128:
    case LITERAL_U8:
    case LITERAL_U16:
    case LITERAL_U32:
    case LITERAL_U64:
    case LITERAL_U128:
    case LITERAL_ISIZE:
    case LITERAL_USIZE: {
        TtNode *node = tt_new(low->tt_arena, TT_INT_LIT, type, loc);
        node->int_lit.value = ast->literal.integer_value;
        node->int_lit.int_kind = (TypeKind)ast->literal.kind;
        return node;
    }
    case LITERAL_F32:
    case LITERAL_F64: {
        TtNode *node = tt_new(low->tt_arena, TT_FLOAT_LIT, type, loc);
        node->float_lit.value = ast->literal.float64_value;
        node->float_lit.float_kind = (TypeKind)ast->literal.kind;
        return node;
    }
    case LITERAL_CHAR: {
        TtNode *node = tt_new(low->tt_arena, TT_CHAR_LIT, type, loc);
        node->char_lit.value = ast->literal.char_value;
        return node;
    }
    case LITERAL_STRING: {
        TtNode *node = tt_new(low->tt_arena, TT_STRING_LIT, type, loc);
        node->string_lit.value = ast->literal.string_value;
        return node;
    }
    case LITERAL_UNIT:
        return tt_new(low->tt_arena, TT_UNIT_LIT, type, loc);
    }
    return tt_new(low->tt_arena, TT_UNIT_LIT, &TYPE_ERROR_INSTANCE, loc);
}

static TtNode *lower_identifier(Lowering *low, const ASTNode *ast) {
    const char *name = ast->identifier.name;
    TtSymbol *symbol = scope_find(low, name);
    if (symbol == NULL) {
        // Create an unresolved symbol with whatever type sema assigned.
        symbol = make_symbol(low, TT_SYM_VAR, name, ast->type, false, ast->location);
        scope_add(low, name, symbol);
    }
    return make_var_ref(low, symbol, ast->location);
}

static TtNode *lower_unary(Lowering *low, const ASTNode *ast) {
    TtNode *operand = lower_expression(low, ast->unary.operand);
    TtNode *node = tt_new(low->tt_arena, TT_UNARY, ast->type, ast->location);
    node->unary.op = ast->unary.op;
    node->unary.operand = operand;
    return node;
}

static TtNode *lower_binary(Lowering *low, const ASTNode *ast) {
    TtNode *left = lower_expression(low, ast->binary.left);
    TtNode *right = lower_expression(low, ast->binary.right);
    TtNode *node = tt_new(low->tt_arena, TT_BINARY, ast->type, ast->location);
    node->binary.op = ast->binary.op;
    node->binary.left = left;
    node->binary.right = right;
    return node;
}

static TtNode *lower_call(Lowering *low, const ASTNode *ast) {
    TtNode *callee = lower_expression(low, ast->call.callee);
    TtNode **args = NULL;
    for (int32_t i = 0; i < BUFFER_LENGTH(ast->call.arguments); i++) {
        TtNode *arg = lower_expression(low, ast->call.arguments[i]);
        BUFFER_PUSH(args, arg);
    }
    TtNode *node = tt_new(low->tt_arena, TT_CALL, ast->type, ast->location);
    node->call.callee = callee;
    node->call.arguments = args;
    return node;
}

static TtNode *lower_member(Lowering *low, const ASTNode *ast) {
    TtNode *object = lower_expression(low, ast->member.object);

    // Tuple member access: tuple.0, tuple.1, ...
    if (object->type != NULL && object->type->kind == TYPE_TUPLE) {
        char *end = NULL;
        long index = strtol(ast->member.member, &end, 10);
        if (end != NULL && *end == '\0') {
            TtNode *node = tt_new(low->tt_arena, TT_TUPLE_INDEX, ast->type, ast->location);
            node->tuple_index.object = object;
            node->tuple_index.element_index = (int32_t)index;
            return node;
        }
    }

    // Module access
    TtNode *node = tt_new(low->tt_arena, TT_MODULE_ACCESS, ast->type, ast->location);
    node->module_access.object = object;
    node->module_access.member = ast->member.member;
    return node;
}

static TtNode *lower_index(Lowering *low, const ASTNode *ast) {
    TtNode *object = lower_expression(low, ast->index_access.object);
    TtNode *index = lower_expression(low, ast->index_access.index);
    TtNode *node = tt_new(low->tt_arena, TT_INDEX, ast->type, ast->location);
    node->index_access.object = object;
    node->index_access.index = index;
    return node;
}

static TtNode *lower_if(Lowering *low, const ASTNode *ast) {
    TtNode *condition = lower_expression(low, ast->if_expression.condition);
    TtNode *then_body = lower_node(low, ast->if_expression.then_body);
    TtNode *else_body = NULL;
    if (ast->if_expression.else_body != NULL) {
        else_body = lower_node(low, ast->if_expression.else_body);
    }
    TtNode *node = tt_new(low->tt_arena, TT_IF, ast->type, ast->location);
    node->if_expr.condition = condition;
    node->if_expr.then_body = then_body;
    node->if_expr.else_body = else_body;
    return node;
}

static TtNode *lower_type_conversion(Lowering *low, const ASTNode *ast) {
    TtNode *operand = lower_expression(low, ast->type_conversion.operand);
    TtNode *node = tt_new(low->tt_arena, TT_TYPE_CONV, ast->type, ast->location);
    node->type_conv.operand = operand;
    node->type_conv.target_type = ast->type;
    return node;
}

static TtNode *lower_array_literal(Lowering *low, const ASTNode *ast) {
    TtNode **elements = NULL;
    for (int32_t i = 0; i < BUFFER_LENGTH(ast->array_literal.elements); i++) {
        TtNode *element = lower_expression(low, ast->array_literal.elements[i]);
        BUFFER_PUSH(elements, element);
    }
    TtNode *node = tt_new(low->tt_arena, TT_ARRAY_LIT, ast->type, ast->location);
    node->array_lit.elements = elements;
    return node;
}

static TtNode *lower_tuple_literal(Lowering *low, const ASTNode *ast) {
    TtNode **elements = NULL;
    for (int32_t i = 0; i < BUFFER_LENGTH(ast->tuple_literal.elements); i++) {
        TtNode *element = lower_expression(low, ast->tuple_literal.elements[i]);
        BUFFER_PUSH(elements, element);
    }
    TtNode *node = tt_new(low->tt_arena, TT_TUPLE_LIT, ast->type, ast->location);
    node->tuple_lit.elements = elements;
    return node;
}

/** Create a TtCall node for a builtin runtime function. */
static TtNode *make_builtin_call(Lowering *low, const char *name, const Type *return_type,
                                 TtNode **args, SourceLocation location) {
    TtSymbol *sym = scope_find(low, name);
    if (sym == NULL) {
        sym = make_symbol(low, TT_SYM_FUNCTION, name, return_type, false, location);
        scope_add(low, name, sym);
    }
    TtNode *callee = make_var_ref(low, sym, location);
    TtNode *node = tt_new(low->tt_arena, TT_CALL, return_type, location);
    node->call.callee = callee;
    node->call.arguments = args;
    return node;
}

/** Return the rsg_string_from_* function name for @p type, or NULL. */
static const char *string_conversion_builtin(const Type *type) {
    if (type == NULL) {
        return NULL;
    }
    switch (type->kind) {
    case TYPE_I32:
        return "rsg_string_from_i32";
    case TYPE_U32:
        return "rsg_string_from_u32";
    case TYPE_I64:
        return "rsg_string_from_i64";
    case TYPE_U64:
        return "rsg_string_from_u64";
    case TYPE_F32:
        return "rsg_string_from_f32";
    case TYPE_F64:
        return "rsg_string_from_f64";
    case TYPE_BOOL:
        return "rsg_string_from_bool";
    case TYPE_CHAR:
        return "rsg_string_from_char";
    default:
        return NULL;
    }
}

/** Lower a string part to a TYPE_STRING TtNode. */
static TtNode *lower_string_part(Lowering *low, const ASTNode *part) {
    TtNode *lowered = lower_expression(low, part);
    if (lowered->type != NULL && lowered->type->kind == TYPE_STRING) {
        return lowered;
    }
    // Wrap in rsg_string_from_TYPE call
    const char *builtin = string_conversion_builtin(lowered->type);
    if (builtin != NULL) {
        TtNode **args = NULL;
        BUFFER_PUSH(args, lowered);
        return make_builtin_call(low, builtin, &TYPE_STRING_INSTANCE, args, part->location);
    }
    return lowered;
}

static TtNode *lower_string_interpolation(Lowering *low, const ASTNode *ast) {
    int32_t part_count = BUFFER_LENGTH(ast->string_interpolation.parts);
    if (part_count == 0) {
        TtNode *node = tt_new(low->tt_arena, TT_STRING_LIT, &TYPE_STRING_INSTANCE, ast->location);
        node->string_lit.value = "";
        return node;
    }

    // Convert all parts to string expressions, skip empty string literals.
    TtNode **string_parts = NULL;
    for (int32_t i = 0; i < part_count; i++) {
        const ASTNode *part = ast->string_interpolation.parts[i];
        if (part->kind == NODE_LITERAL && part->literal.kind == LITERAL_STRING) {
            if (part->literal.string_value == NULL || part->literal.string_value[0] == '\0') {
                continue;
            }
        }
        BUFFER_PUSH(string_parts, lower_string_part(low, part));
    }

    int32_t count = BUFFER_LENGTH(string_parts);
    if (count == 0) {
        TtNode *node = tt_new(low->tt_arena, TT_STRING_LIT, &TYPE_STRING_INSTANCE, ast->location);
        node->string_lit.value = "";
        BUFFER_FREE(string_parts);
        return node;
    }
    if (count == 1) {
        TtNode *result = string_parts[0];
        BUFFER_FREE(string_parts);
        return result;
    }

    // Chain with rsg_string_concat: concat(concat(a, b), c)
    TtNode *result = string_parts[0];
    for (int32_t i = 1; i < count; i++) {
        TtNode **args = NULL;
        BUFFER_PUSH(args, result);
        BUFFER_PUSH(args, string_parts[i]);
        result =
            make_builtin_call(low, "rsg_string_concat", &TYPE_STRING_INSTANCE, args, ast->location);
    }
    BUFFER_FREE(string_parts);
    return result;
}

static TtNode *lower_expression(Lowering *low, const ASTNode *ast) {
    if (ast == NULL) {
        return NULL;
    }
    switch (ast->kind) {
    case NODE_LITERAL:
        return lower_literal(low, ast);
    case NODE_IDENTIFIER:
        return lower_identifier(low, ast);
    case NODE_UNARY:
        return lower_unary(low, ast);
    case NODE_BINARY:
        return lower_binary(low, ast);
    case NODE_CALL:
        return lower_call(low, ast);
    case NODE_MEMBER:
        return lower_member(low, ast);
    case NODE_INDEX:
        return lower_index(low, ast);
    case NODE_IF:
        return lower_if(low, ast);
    case NODE_BLOCK:
        return lower_block(low, ast);
    case NODE_TYPE_CONVERSION:
        return lower_type_conversion(low, ast);
    case NODE_ARRAY_LITERAL:
        return lower_array_literal(low, ast);
    case NODE_TUPLE_LITERAL:
        return lower_tuple_literal(low, ast);
    case NODE_STRING_INTERPOLATION:
        return lower_string_interpolation(low, ast);
    default:
        break;
    }
    return tt_new(low->tt_arena, TT_UNIT_LIT, &TYPE_ERROR_INSTANCE, ast->location);
}

// ── Statement / declaration lowering ──────────────────────────────────

static TtNode *lower_variable_declaration(Lowering *low, const ASTNode *ast) {
    const char *name = ast->variable_declaration.name;
    const Type *type = ast->type != NULL ? ast->type : &TYPE_ERROR_INSTANCE;
    bool is_mut = ast->variable_declaration.is_variable;

    TtSymbol *symbol = make_symbol(low, TT_SYM_VAR, name, type, is_mut, ast->location);
    scope_add(low, name, symbol);

    TtNode *init = NULL;
    if (ast->variable_declaration.initializer != NULL) {
        init = lower_expression(low, ast->variable_declaration.initializer);
    }

    TtNode *node = tt_new(low->tt_arena, TT_VAR_DECL, &TYPE_UNIT_INSTANCE, ast->location);
    node->var_decl.symbol = symbol;
    node->var_decl.name = name;
    node->var_decl.var_type = type;
    node->var_decl.initializer = init;
    node->var_decl.is_mut = is_mut;
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
    TokenKind base_op = compound_to_base_op(ast->compound_assign.op);
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

static TtNode *lower_function_declaration(Lowering *low, const ASTNode *ast) {
    const char *name = ast->function_declaration.name;
    bool is_public = ast->function_declaration.is_public;
    const Type *return_type = ast->type != NULL ? ast->type : &TYPE_UNIT_INSTANCE;

    TtSymbol *func_sym = make_symbol(low, TT_SYM_FUNCTION, name, return_type, false, ast->location);
    scope_add(low, name, func_sym);

    scope_enter(low);

    // Lower parameters
    TtNode **params = NULL;
    for (int32_t i = 0; i < BUFFER_LENGTH(ast->function_declaration.parameters); i++) {
        const ASTNode *param_ast = ast->function_declaration.parameters[i];
        const char *param_name = param_ast->parameter.name;
        const Type *param_type = param_ast->type != NULL ? param_ast->type : &TYPE_ERROR_INSTANCE;

        TtSymbol *param_sym =
            make_symbol(low, TT_SYM_PARAM, param_name, param_type, false, param_ast->location);
        scope_add(low, param_name, param_sym);

        TtNode *param_node = tt_new(low->tt_arena, TT_PARAM, param_type, param_ast->location);
        param_node->param.symbol = param_sym;
        param_node->param.name = param_name;
        param_node->param.param_type = param_type;
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

    scope_leave(low);

    TtNode *node = tt_new(low->tt_arena, TT_FUNCTION_DECL, &TYPE_UNIT_INSTANCE, ast->location);
    node->function_decl.name = name;
    node->function_decl.is_public = is_public;
    node->function_decl.symbol = func_sym;
    node->function_decl.params = params;
    node->function_decl.return_type = return_type;
    node->function_decl.body = body;
    return node;
}

static TtNode *lower_block(Lowering *low, const ASTNode *ast) {
    if (ast == NULL) {
        return NULL;
    }
    assert(ast->kind == NODE_BLOCK);
    scope_enter(low);

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

    scope_leave(low);

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

    scope_enter(low);

    // var _end = end
    const char *end_name = make_temp_name(low);
    TtSymbol *end_sym = make_symbol(low, TT_SYM_VAR, end_name, iter_type, false, loc);
    scope_add(low, end_name, end_sym);
    TtNode *end_init = lower_expression(low, ast->for_loop.end);
    TtNode *end_decl = tt_new(low->tt_arena, TT_VAR_DECL, &TYPE_UNIT_INSTANCE, loc);
    end_decl->var_decl.symbol = end_sym;
    end_decl->var_decl.name = end_name;
    end_decl->var_decl.var_type = iter_type;
    end_decl->var_decl.initializer = end_init;
    end_decl->var_decl.is_mut = false;

    // var i = start
    const char *var_name = ast->for_loop.variable_name;
    TtSymbol *var_sym = make_symbol(low, TT_SYM_VAR, var_name, iter_type, true, loc);
    scope_add(low, var_name, var_sym);
    TtNode *start_init = lower_expression(low, ast->for_loop.start);
    TtNode *var_decl = tt_new(low->tt_arena, TT_VAR_DECL, &TYPE_UNIT_INSTANCE, loc);
    var_decl->var_decl.symbol = var_sym;
    var_decl->var_decl.name = var_name;
    var_decl->var_decl.var_type = iter_type;
    var_decl->var_decl.initializer = start_init;
    var_decl->var_decl.is_mut = true;

    // Loop body statements
    TtNode **loop_stmts = NULL;

    // if var >= _end { break }
    TtNode *cmp = tt_new(low->tt_arena, TT_BINARY, &TYPE_BOOL_INSTANCE, loc);
    cmp->binary.op = TOKEN_GREATER_EQUAL;
    cmp->binary.left = make_var_ref(low, var_sym, loc);
    cmp->binary.right = make_var_ref(low, end_sym, loc);

    TtNode *break_node = tt_new(low->tt_arena, TT_BREAK, &TYPE_UNIT_INSTANCE, loc);

    TtNode **break_stmts = NULL;
    BUFFER_PUSH(break_stmts, break_node);
    TtNode *break_block = tt_new(low->tt_arena, TT_BLOCK, &TYPE_UNIT_INSTANCE, loc);
    break_block->block.statements = break_stmts;

    TtNode *if_break = tt_new(low->tt_arena, TT_IF, &TYPE_UNIT_INSTANCE, loc);
    if_break->if_expr.condition = cmp;
    if_break->if_expr.then_body = break_block;

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
    increment->binary.left = make_var_ref(low, var_sym, loc);
    increment->binary.right = make_int_lit(low, 1, iter_type, TYPE_I32, loc);

    TtNode *inc_assign = tt_new(low->tt_arena, TT_ASSIGN, &TYPE_UNIT_INSTANCE, loc);
    inc_assign->assign.target = make_var_ref(low, var_sym, loc);
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

    scope_leave(low);

    TtNode *outer = tt_new(low->tt_arena, TT_BLOCK, &TYPE_UNIT_INSTANCE, loc);
    outer->block.statements = outer_stmts;
    return outer;
}

static TtNode *lower_expression_statement(Lowering *low, const ASTNode *ast) {
    TtNode *expr = lower_expression(low, ast->expression_statement.expression);
    TtNode *node = tt_new(low->tt_arena, TT_EXPR_STMT, &TYPE_UNIT_INSTANCE, ast->location);
    node->expr_stmt.expression = expr;
    return node;
}

static TtNode *lower_node(Lowering *low, const ASTNode *ast) {
    if (ast == NULL) {
        return NULL;
    }

    switch (ast->kind) {
    case NODE_FILE: {
        scope_enter(low);

        // Pre-register all functions in scope before lowering bodies
        for (int32_t i = 0; i < BUFFER_LENGTH(ast->file.declarations); i++) {
            const ASTNode *decl = ast->file.declarations[i];
            if (decl->kind == NODE_FUNCTION_DECLARATION) {
                const Type *ret = decl->type != NULL ? decl->type : &TYPE_UNIT_INSTANCE;
                TtSymbol *sym = make_symbol(low, TT_SYM_FUNCTION, decl->function_declaration.name,
                                            ret, false, decl->location);
                scope_add(low, decl->function_declaration.name, sym);
            }
        }

        TtNode **declarations = NULL;
        for (int32_t i = 0; i < BUFFER_LENGTH(ast->file.declarations); i++) {
            TtNode *decl = lower_node(low, ast->file.declarations[i]);
            if (decl != NULL) {
                BUFFER_PUSH(declarations, decl);
            }
        }
        scope_leave(low);

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
        return lower_if(low, ast);

    default:
        // Expressions at statement level
        return lower_expression(low, ast);
    }
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
    return low;
}

void lowering_destroy(Lowering *lowering) {
    free(lowering);
}

TtNode *lowering_lower(Lowering *lowering, const ASTNode *file) {
    return lower_node(lowering, file);
}
