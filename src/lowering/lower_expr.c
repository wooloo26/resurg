#include "_lowering.h"

// ── Scope manipulation ────────────────────────────────────────────────

void lowering_scope_enter(Lowering *low) {
    LoweringScope *scope = arena_alloc(low->tt_arena, sizeof(LoweringScope));
    hash_table_init(&scope->table, low->tt_arena);
    scope->parent = low->scope;
    low->scope = scope;
}

void lowering_scope_leave(Lowering *low) {
    low->scope = low->scope->parent;
}

void lowering_scope_add(Lowering *low, const char *name, TtSymbol *symbol) {
    hash_table_insert(&low->scope->table, name, symbol);
}

TtSymbol *lowering_scope_find(const Lowering *low, const char *name) {
    for (LoweringScope *scope = low->scope; scope != NULL; scope = scope->parent) {
        TtSymbol *symbol = hash_table_lookup(&scope->table, name);
        if (symbol != NULL) {
            return symbol;
        }
    }
    return NULL;
}

// ── Shared helpers ────────────────────────────────────────────────────

TtSymbol *lowering_make_symbol(Lowering *low, TtSymbolKind kind, const char *name, const Type *type,
                               bool is_mut, SourceLocation location) {
    Symbol *sema_sym = arena_alloc(low->sema_arena, sizeof(Symbol));
    memset(sema_sym, 0, sizeof(Symbol));
    sema_sym->name = name;
    sema_sym->type = type;
    switch (kind) {
    case TT_SYMBOL_VARIABLE:
        sema_sym->kind = SYM_VAR;
        break;
    case TT_SYMBOL_PARAMETER:
        sema_sym->kind = SYM_PARAM;
        break;
    case TT_SYMBOL_FUNCTION:
        sema_sym->kind = SYM_FUNCTION;
        break;
    case TT_SYMBOL_TYPE:
        sema_sym->kind = SYM_TYPE;
        break;
    case TT_SYMBOL_MODULE:
        sema_sym->kind = SYM_MODULE;
        break;
    }
    return tt_symbol_new(low->tt_arena, kind, sema_sym, is_mut, location);
}

const char *lowering_make_temp_name(Lowering *low) {
    return arena_sprintf(low->tt_arena, "_tt_tmp_%d", low->temp_counter++);
}

TtNode *lowering_make_var_ref(Lowering *low, TtSymbol *symbol, SourceLocation location) {
    TtNode *node = tt_new(low->tt_arena, TT_VARIABLE_REFERENCE, tt_symbol_type(symbol), location);
    node->variable_reference.symbol = symbol;
    return node;
}

TtNode *lowering_make_int_lit(Lowering *low, uint64_t value, const Type *type, TypeKind int_kind,
                              SourceLocation location) {
    TtNode *node = tt_new(low->tt_arena, TT_INT_LITERAL, type, location);
    node->int_literal.value = value;
    node->int_literal.int_kind = int_kind;
    return node;
}

TokenKind lowering_compound_to_base_op(TokenKind op) {
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

TtNode *lowering_make_builtin_call(Lowering *low, const char *name, const Type *return_type,
                                   TtNode **args, SourceLocation location) {
    TtSymbol *sym = lowering_scope_find(low, name);
    if (sym == NULL) {
        sym = lowering_make_symbol(low, TT_SYMBOL_FUNCTION, name, return_type, false, location);
        lowering_scope_add(low, name, sym);
    }
    TtNode *callee = lowering_make_var_ref(low, sym, location);
    TtNode *node = tt_new(low->tt_arena, TT_CALL, return_type, location);
    node->call.callee = callee;
    node->call.arguments = args;
    return node;
}

// ── Expression lowering ───────────────────────────────────────────────

static TtNode *lower_literal(Lowering *low, const ASTNode *ast) {
    SourceLocation loc = ast->location;
    const Type *type = ast->type;

    switch (ast->literal.kind) {
    case LITERAL_BOOL: {
        TtNode *node = tt_new(low->tt_arena, TT_BOOL_LITERAL, type, loc);
        node->bool_literal.value = ast->literal.boolean_value;
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
        TtNode *node = tt_new(low->tt_arena, TT_INT_LITERAL, type, loc);
        node->int_literal.value = ast->literal.integer_value;
        node->int_literal.int_kind = (TypeKind)ast->literal.kind;
        return node;
    }
    case LITERAL_F32:
    case LITERAL_F64: {
        TtNode *node = tt_new(low->tt_arena, TT_FLOAT_LITERAL, type, loc);
        node->float_literal.value = ast->literal.float64_value;
        node->float_literal.float_kind = (TypeKind)ast->literal.kind;
        return node;
    }
    case LITERAL_CHAR: {
        TtNode *node = tt_new(low->tt_arena, TT_CHAR_LITERAL, type, loc);
        node->char_literal.value = ast->literal.char_value;
        return node;
    }
    case LITERAL_STRING: {
        TtNode *node = tt_new(low->tt_arena, TT_STRING_LITERAL, type, loc);
        node->string_literal.value = ast->literal.string_value;
        return node;
    }
    case LITERAL_UNIT:
        return tt_new(low->tt_arena, TT_UNIT_LITERAL, type, loc);
    }
    return tt_new(low->tt_arena, TT_UNIT_LITERAL, &TYPE_ERROR_INSTANCE, loc);
}

static TtNode *lower_identifier(Lowering *low, const ASTNode *ast) {
    const char *name = ast->identifier.name;
    TtSymbol *symbol = lowering_scope_find(low, name);
    if (symbol == NULL) {
        // Create an unresolved symbol with whatever type sema assigned.
        symbol =
            lowering_make_symbol(low, TT_SYMBOL_VARIABLE, name, ast->type, false, ast->location);
        lowering_scope_add(low, name, symbol);
    }
    return lowering_make_var_ref(low, symbol, ast->location);
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

static TtNode *lower_type_conversion(Lowering *low, const ASTNode *ast) {
    TtNode *operand = lower_expression(low, ast->type_conversion.operand);
    TtNode *node = tt_new(low->tt_arena, TT_TYPE_CONVERSION, ast->type, ast->location);
    node->type_conversion.operand = operand;
    node->type_conversion.target_type = ast->type;
    return node;
}

static TtNode *lower_array_literal(Lowering *low, const ASTNode *ast) {
    TtNode **elements = NULL;
    for (int32_t i = 0; i < BUFFER_LENGTH(ast->array_literal.elements); i++) {
        TtNode *element = lower_expression(low, ast->array_literal.elements[i]);
        BUFFER_PUSH(elements, element);
    }
    TtNode *node = tt_new(low->tt_arena, TT_ARRAY_LITERAL, ast->type, ast->location);
    node->array_literal.elements = elements;
    return node;
}

static TtNode *lower_tuple_literal(Lowering *low, const ASTNode *ast) {
    TtNode **elements = NULL;
    for (int32_t i = 0; i < BUFFER_LENGTH(ast->tuple_literal.elements); i++) {
        TtNode *element = lower_expression(low, ast->tuple_literal.elements[i]);
        BUFFER_PUSH(elements, element);
    }
    TtNode *node = tt_new(low->tt_arena, TT_TUPLE_LITERAL, ast->type, ast->location);
    node->tuple_literal.elements = elements;
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
        return lowering_make_builtin_call(low, builtin, &TYPE_STRING_INSTANCE, args,
                                          part->location);
    }
    return lowered;
}

static TtNode *lower_string_interpolation(Lowering *low, const ASTNode *ast) {
    int32_t part_count = BUFFER_LENGTH(ast->string_interpolation.parts);
    if (part_count == 0) {
        TtNode *node =
            tt_new(low->tt_arena, TT_STRING_LITERAL, &TYPE_STRING_INSTANCE, ast->location);
        node->string_literal.value = "";
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
        TtNode *node =
            tt_new(low->tt_arena, TT_STRING_LITERAL, &TYPE_STRING_INSTANCE, ast->location);
        node->string_literal.value = "";
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
        result = lowering_make_builtin_call(low, "rsg_string_concat", &TYPE_STRING_INSTANCE, args,
                                            ast->location);
    }
    BUFFER_FREE(string_parts);
    return result;
}

TtNode *lower_expression(Lowering *low, const ASTNode *ast) {
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
        return lower_statement_if(low, ast);
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
    return tt_new(low->tt_arena, TT_UNIT_LITERAL, &TYPE_ERROR_INSTANCE, ast->location);
}
