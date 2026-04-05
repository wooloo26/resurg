#include "_lowering.h"

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

/** Negate a boolean expression with a unary `!`. */
static TtNode *make_bool_negate(Lowering *low, TtNode *expr, SourceLocation loc) {
    TtNode *neg = tt_new(low->tt_arena, TT_UNARY, &TYPE_BOOL_INSTANCE, loc);
    neg->unary.op = TOKEN_BANG;
    neg->unary.operand = expr;
    return neg;
}

/**
 * Build an element comparison for equality expansion.
 *
 * For TYPE_STRING elements, emits rsg_string_equal(l, r) (negated for !=).
 * Otherwise emits a plain binary comparison.
 */
static TtNode *make_element_comparison(Lowering *low, TtNode *l_elem, TtNode *r_elem,
                                       const Type *elem_type, TokenKind elem_op, bool is_equal,
                                       SourceLocation loc) {
    if (elem_type->kind == TYPE_STRING) {
        TtNode **args = NULL;
        BUFFER_PUSH(args, l_elem);
        BUFFER_PUSH(args, r_elem);
        TtNode *cmp =
            lowering_make_builtin_call(low, "rsg_string_equal", &TYPE_BOOL_INSTANCE, args, loc);
        if (!is_equal) {
            cmp = make_bool_negate(low, cmp, loc);
        }
        return cmp;
    }
    TtNode *cmp = tt_new(low->tt_arena, TT_BINARY, &TYPE_BOOL_INSTANCE, loc);
    cmp->binary.op = elem_op;
    cmp->binary.left = l_elem;
    cmp->binary.right = r_elem;
    return cmp;
}

/** Join @p cmp into the running @p result chain with @p join_op. */
static TtNode *chain_comparison(Lowering *low, TtNode *result, TtNode *cmp, TokenKind join_op,
                                SourceLocation loc) {
    if (result == NULL) {
        return cmp;
    }
    TtNode *join = tt_new(low->tt_arena, TT_BINARY, &TYPE_BOOL_INSTANCE, loc);
    join->binary.op = join_op;
    join->binary.left = result;
    join->binary.right = cmp;
    return join;
}

/** Build an array element access: obj[i]. */
static TtNode *make_array_element_access(Lowering *low, TtSymbol *sym, int32_t index,
                                         const Type *elem_type, SourceLocation loc) {
    TtNode *ref = lowering_make_var_ref(low, sym, loc);
    TtNode *idx = lowering_make_int_lit(low, (uint64_t)index, &TYPE_I32_INSTANCE, TYPE_I32, loc);
    TtNode *elem = tt_new(low->tt_arena, TT_INDEX, elem_type, loc);
    elem->index_access.object = ref;
    elem->index_access.index = idx;
    return elem;
}

/** Build a tuple element access: obj.N. */
static TtNode *make_tuple_element_access(Lowering *low, TtSymbol *sym, int32_t index,
                                         const Type *elem_type, SourceLocation loc) {
    TtNode *ref = lowering_make_var_ref(low, sym, loc);
    TtNode *elem = tt_new(low->tt_arena, TT_TUPLE_INDEX, elem_type, loc);
    elem->tuple_index.object = ref;
    elem->tuple_index.element_index = index;
    return elem;
}

/**
 * Expand array or tuple equality into element-wise comparison.
 *
 * Array:  `a == b` → `{ var l=a; var r=b; l[0]==r[0] && l[1]==r[1] && ... }`
 * Tuple:  `t == u` → `{ var l=t; var r=u; l.0==r.0 && l.1==r.1 && ... }`
 */
static TtNode *lower_aggregate_equality(Lowering *low, TtNode *left, TtNode *right,
                                        const Type *type, TokenKind op, SourceLocation loc) {
    bool is_equal = (op == TOKEN_EQUAL_EQUAL);
    TokenKind elem_op = is_equal ? TOKEN_EQUAL_EQUAL : TOKEN_BANG_EQUAL;
    TokenKind join_op = is_equal ? TOKEN_AMPERSAND_AMPERSAND : TOKEN_PIPE_PIPE;
    bool is_array = (type->kind == TYPE_ARRAY);
    int32_t count = is_array ? type->array.size : type->tuple.count;

    // Store operands in temporaries to avoid re-evaluation
    const char *left_name = lowering_make_temp_name(low);
    TtSymbol *left_sym = lowering_make_symbol(low, TT_SYMBOL_VARIABLE, left_name, type, false, loc);
    lowering_scope_add(low, left_name, left_sym);

    const char *right_name = lowering_make_temp_name(low);
    TtSymbol *right_sym =
        lowering_make_symbol(low, TT_SYMBOL_VARIABLE, right_name, type, false, loc);
    lowering_scope_add(low, right_name, right_sym);

    TtNode *result = NULL;
    for (int32_t i = 0; i < count; i++) {
        const Type *elem_type;
        TtNode *l_elem, *r_elem;

        if (is_array) {
            elem_type = type->array.element;
            l_elem = make_array_element_access(low, left_sym, i, elem_type, loc);
            r_elem = make_array_element_access(low, right_sym, i, elem_type, loc);
        } else {
            elem_type = type->tuple.elements[i];
            l_elem = make_tuple_element_access(low, left_sym, i, elem_type, loc);
            r_elem = make_tuple_element_access(low, right_sym, i, elem_type, loc);
        }

        TtNode *cmp =
            make_element_comparison(low, l_elem, r_elem, elem_type, elem_op, is_equal, loc);
        result = chain_comparison(low, result, cmp, join_op, loc);
    }

    if (result == NULL) {
        TtNode *lit = tt_new(low->tt_arena, TT_BOOL_LITERAL, &TYPE_BOOL_INSTANCE, loc);
        lit->bool_literal.value = is_equal;
        return lit;
    }

    TtNode **stmts = NULL;
    BUFFER_PUSH(stmts, lowering_make_var_decl(low, left_sym, left_name, type, left, false, loc));
    BUFFER_PUSH(stmts, lowering_make_var_decl(low, right_sym, right_name, type, right, false, loc));

    TtNode *block = tt_new(low->tt_arena, TT_BLOCK, &TYPE_BOOL_INSTANCE, loc);
    block->block.statements = stmts;
    block->block.result = result;
    return block;
}

/** Lower string equality: rsg_string_equal(l, r), negated for !=. */
static TtNode *lower_string_equality(Lowering *low, TtNode *left, TtNode *right, TokenKind op,
                                     SourceLocation loc) {
    TtNode **args = NULL;
    BUFFER_PUSH(args, left);
    BUFFER_PUSH(args, right);
    TtNode *call =
        lowering_make_builtin_call(low, "rsg_string_equal", &TYPE_BOOL_INSTANCE, args, loc);
    if (op == TOKEN_BANG_EQUAL) {
        return make_bool_negate(low, call, loc);
    }
    return call;
}

static TtNode *lower_binary(Lowering *low, const ASTNode *ast) {
    TtNode *left = lower_expression(low, ast->binary.left);
    TtNode *right = lower_expression(low, ast->binary.right);
    TokenKind op = ast->binary.op;
    const Type *left_type = left->type;

    // String equality/inequality → rsg_string_equal call
    if (left_type != NULL && left_type->kind == TYPE_STRING &&
        (op == TOKEN_EQUAL_EQUAL || op == TOKEN_BANG_EQUAL)) {
        return lower_string_equality(low, left, right, op, ast->location);
    }

    // Array/Tuple equality/inequality → element-wise comparison
    if (left_type != NULL && (left_type->kind == TYPE_ARRAY || left_type->kind == TYPE_TUPLE) &&
        (op == TOKEN_EQUAL_EQUAL || op == TOKEN_BANG_EQUAL)) {
        return lower_aggregate_equality(low, left, right, left_type, op, ast->location);
    }

    TtNode *node = tt_new(low->tt_arena, TT_BINARY, ast->type, ast->location);
    node->binary.op = op;
    node->binary.left = left;
    node->binary.right = right;
    return node;
}

/** Return true if the callee AST node resolves to the builtin "assert". */
static bool is_assert_callee(const ASTNode *callee) {
    if (callee->kind != NODE_IDENTIFIER) {
        return false;
    }
    return strcmp(callee->identifier.name, "assert") == 0;
}

/**
 * Expand assert(cond, msg?) → rsg_assert(cond, msg_or_null, file, line).
 *
 * When the message is a string literal, pass its raw value directly as a
 * TT_STRING_LITERAL so codegen can emit a plain C string.  For non-literal
 * string expressions, lowering wraps them unchanged — codegen extracts
 * `.data` from the emitted rsg_string.
 */
static TtNode *lower_assert_call(Lowering *low, const ASTNode *ast) {
    int32_t arg_count = BUFFER_LENGTH(ast->call.arguments);
    SourceLocation loc = ast->location;

    // Condition (default to false if missing)
    TtNode *condition;
    if (arg_count > 0) {
        condition = lower_expression(low, ast->call.arguments[0]);
    } else {
        condition = tt_new(low->tt_arena, TT_BOOL_LITERAL, &TYPE_BOOL_INSTANCE, loc);
        condition->bool_literal.value = false;
    }

    // Message (NULL when absent; string literal kept as literal, expression passed through)
    TtNode *message;
    if (arg_count > 1) {
        message = lower_expression(low, ast->call.arguments[1]);
    } else {
        // Pass NULL sentinel — codegen emits "NULL" for this
        message = tt_new(low->tt_arena, TT_UNIT_LITERAL, &TYPE_UNIT_INSTANCE, loc);
    }

    // File name as string literal
    TtNode *file_node = tt_new(low->tt_arena, TT_STRING_LITERAL, &TYPE_STRING_INSTANCE, loc);
    file_node->string_literal.value = loc.file != NULL ? loc.file : "<unknown>";

    // Line number as i32 literal
    TtNode *line_node =
        lowering_make_int_lit(low, (uint64_t)loc.line, &TYPE_I32_INSTANCE, TYPE_I32, loc);

    TtNode **args = NULL;
    BUFFER_PUSH(args, condition);
    BUFFER_PUSH(args, message);
    BUFFER_PUSH(args, file_node);
    BUFFER_PUSH(args, line_node);

    return lowering_make_builtin_call(low, "rsg_assert", &TYPE_UNIT_INSTANCE, args, loc);
}

/** Lower a buffer of AST expression nodes into a buffer of TT nodes. */
static TtNode **lower_element_list(Lowering *low, ASTNode **ast_elements) {
    TtNode **elements = NULL;
    for (int32_t i = 0; i < BUFFER_LENGTH(ast_elements); i++) {
        BUFFER_PUSH(elements, lower_expression(low, ast_elements[i]));
    }
    return elements;
}

static TtNode *lower_call(Lowering *low, const ASTNode *ast) {
    // Assert expansion: assert(cond, msg?) → rsg_assert(cond, msg, file, line)
    if (is_assert_callee(ast->call.callee)) {
        return lower_assert_call(low, ast);
    }

    // Method call: obj.method(args)
    if (ast->call.callee->kind == NODE_MEMBER) {
        const ASTNode *member_ast = ast->call.callee;
        const Type *obj_type = member_ast->member.object->type;

        // Auto-deref pointer for method calls
        bool via_pointer = false;
        if (obj_type != NULL && obj_type->kind == TYPE_POINTER) {
            obj_type = obj_type->pointer.pointee;
            via_pointer = true;
        }

        if (obj_type != NULL && obj_type->kind == TYPE_STRUCT) {
            const char *method_name = member_ast->member.member;
            TtNode *receiver = lower_expression(low, member_ast->member.object);

            if (!via_pointer && low->current_receiver != NULL &&
                receiver->kind == TT_VARIABLE_REFERENCE &&
                receiver->variable_reference.symbol == low->current_receiver) {
                via_pointer = true;
            }

            // Look up method — direct first, then promoted from embedded
            const char *key =
                arena_sprintf(low->tt_arena, "%s.%s", obj_type->struct_type.name, method_name);
            TtSymbol *method_sym = lowering_scope_find(low, key);

            if (method_sym == NULL) {
                for (int32_t i = 0; i < obj_type->struct_type.embed_count; i++) {
                    const Type *embed_type = obj_type->struct_type.embedded[i];
                    const char *embed_key = arena_sprintf(
                        low->tt_arena, "%s.%s", embed_type->struct_type.name, method_name);
                    method_sym = lowering_scope_find(low, embed_key);
                    if (method_sym != NULL) {
                        TtNode *embed_access = tt_new(low->tt_arena, TT_STRUCT_FIELD_ACCESS,
                                                      embed_type, receiver->location);
                        embed_access->struct_field_access.object = receiver;
                        embed_access->struct_field_access.field = embed_type->struct_type.name;
                        embed_access->struct_field_access.via_pointer = via_pointer;
                        receiver = embed_access;
                        break;
                    }
                }
            }

            if (method_sym != NULL) {
                TtNode **args = lower_element_list(low, ast->call.arguments);
                TtNode *node = tt_new(low->tt_arena, TT_METHOD_CALL, ast->type, ast->location);
                node->method_call.receiver = receiver;
                node->method_call.mangled_name = method_sym->mangled_name;
                node->method_call.arguments = args;
                return node;
            }
        }
    }

    TtNode *callee = lower_expression(low, ast->call.callee);
    TtNode **args = lower_element_list(low, ast->call.arguments);
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

    // Auto-deref for pointer types: p.field → p->field
    bool via_pointer = false;
    const Type *lookup_type = object->type;
    if (lookup_type != NULL && lookup_type->kind == TYPE_POINTER) {
        lookup_type = lookup_type->pointer.pointee;
        via_pointer = true;
    }

    // Struct field access
    if (lookup_type != NULL && lookup_type->kind == TYPE_STRUCT) {
        const char *field_name = ast->member.member;

        if (!via_pointer && low->current_receiver != NULL &&
            object->kind == TT_VARIABLE_REFERENCE &&
            object->variable_reference.symbol == low->current_receiver) {
            via_pointer = true;
        }

        // Direct field
        const StructField *sf = type_struct_find_field(lookup_type, field_name);
        if (sf != NULL) {
            TtNode *node = tt_new(low->tt_arena, TT_STRUCT_FIELD_ACCESS, ast->type, ast->location);
            node->struct_field_access.object = object;
            node->struct_field_access.field = field_name;
            node->struct_field_access.via_pointer = via_pointer;
            return node;
        }

        // Promoted field from embedded struct
        for (int32_t i = 0; i < lookup_type->struct_type.embed_count; i++) {
            const Type *embed_type = lookup_type->struct_type.embedded[i];
            if (type_struct_find_field(embed_type, field_name) != NULL) {
                TtNode *embed_access =
                    tt_new(low->tt_arena, TT_STRUCT_FIELD_ACCESS, embed_type, ast->location);
                embed_access->struct_field_access.object = object;
                embed_access->struct_field_access.field = embed_type->struct_type.name;
                embed_access->struct_field_access.via_pointer = via_pointer;

                TtNode *field_node =
                    tt_new(low->tt_arena, TT_STRUCT_FIELD_ACCESS, ast->type, ast->location);
                field_node->struct_field_access.object = embed_access;
                field_node->struct_field_access.field = field_name;
                field_node->struct_field_access.via_pointer = false;
                return field_node;
            }
        }

        // Fallback (shouldn't reach after sema)
        TtNode *node = tt_new(low->tt_arena, TT_STRUCT_FIELD_ACCESS, ast->type, ast->location);
        node->struct_field_access.object = object;
        node->struct_field_access.field = field_name;
        node->struct_field_access.via_pointer = via_pointer;
        return node;
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
    TtNode **elements = lower_element_list(low, ast->array_literal.elements);
    TtNode *node = tt_new(low->tt_arena, TT_ARRAY_LITERAL, ast->type, ast->location);
    node->array_literal.elements = elements;
    return node;
}

static TtNode *lower_tuple_literal(Lowering *low, const ASTNode *ast) {
    TtNode **elements = lower_element_list(low, ast->tuple_literal.elements);
    TtNode *node = tt_new(low->tt_arena, TT_TUPLE_LITERAL, ast->type, ast->location);
    node->tuple_literal.elements = elements;
    return node;
}

// String interpolation lowering is in lower_string.c

static TtNode *lower_struct_literal(Lowering *low, const ASTNode *ast) {
    const Type *struct_type = ast->type;
    if (struct_type == NULL || struct_type->kind != TYPE_STRUCT) {
        return tt_new(low->tt_arena, TT_UNIT_LITERAL, &TYPE_ERROR_INSTANCE, ast->location);
    }

    const char **field_names = NULL;
    TtNode **field_values = NULL;

    for (int32_t fi = 0; fi < struct_type->struct_type.field_count; fi++) {
        const StructField *sf = &struct_type->struct_type.fields[fi];

        // Check if this field is an embedded struct
        bool is_embedded = false;
        for (int32_t ei = 0; ei < struct_type->struct_type.embed_count; ei++) {
            if (strcmp(struct_type->struct_type.embedded[ei]->struct_type.name, sf->name) == 0) {
                is_embedded = true;
                break;
            }
        }

        if (is_embedded && sf->type->kind == TYPE_STRUCT) {
            // Check if directly provided as "Base = Base { ... }"
            bool directly_provided = false;
            for (int32_t ai = 0; ai < BUFFER_LENGTH(ast->struct_literal.field_names); ai++) {
                if (strcmp(ast->struct_literal.field_names[ai], sf->name) == 0) {
                    BUFFER_PUSH(field_names, sf->name);
                    BUFFER_PUSH(field_values,
                                lower_expression(low, ast->struct_literal.field_values[ai]));
                    directly_provided = true;
                    break;
                }
            }
            if (!directly_provided) {
                // Collect promoted fields from AST that belong to this embedded type
                const Type *embed_type = sf->type;
                const char **sub_names = NULL;
                TtNode **sub_values = NULL;

                for (int32_t ej = 0; ej < embed_type->struct_type.field_count; ej++) {
                    const char *embed_field = embed_type->struct_type.fields[ej].name;
                    for (int32_t ai = 0; ai < BUFFER_LENGTH(ast->struct_literal.field_names);
                         ai++) {
                        if (strcmp(ast->struct_literal.field_names[ai], embed_field) == 0) {
                            BUFFER_PUSH(sub_names, embed_field);
                            BUFFER_PUSH(sub_values, lower_expression(
                                                        low, ast->struct_literal.field_values[ai]));
                            break;
                        }
                    }
                }

                if (BUFFER_LENGTH(sub_names) > 0) {
                    TtNode *sub =
                        tt_new(low->tt_arena, TT_STRUCT_LITERAL, embed_type, ast->location);
                    sub->struct_literal.field_names = sub_names;
                    sub->struct_literal.field_values = sub_values;
                    BUFFER_PUSH(field_names, sf->name);
                    BUFFER_PUSH(field_values, sub);
                }
            }
        } else {
            // Regular field — look it up in the AST
            for (int32_t ai = 0; ai < BUFFER_LENGTH(ast->struct_literal.field_names); ai++) {
                if (strcmp(ast->struct_literal.field_names[ai], sf->name) == 0) {
                    BUFFER_PUSH(field_names, sf->name);
                    BUFFER_PUSH(field_values,
                                lower_expression(low, ast->struct_literal.field_values[ai]));
                    break;
                }
            }
        }
    }

    TtNode *node = tt_new(low->tt_arena, TT_STRUCT_LITERAL, struct_type, ast->location);
    node->struct_literal.field_names = field_names;
    node->struct_literal.field_values = field_values;
    return node;
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
    case NODE_STRUCT_LITERAL:
        return lower_struct_literal(low, ast);
    case NODE_ADDRESS_OF: {
        TtNode *operand = lower_expression(low, ast->address_of.operand);
        // &StructLiteral{} → heap allocation
        if (ast->address_of.operand->kind == NODE_STRUCT_LITERAL) {
            TtNode *node = tt_new(low->tt_arena, TT_HEAP_ALLOC, ast->type, ast->location);
            node->heap_alloc.operand = operand;
            return node;
        }
        // &variable → native address-of
        TtNode *node = tt_new(low->tt_arena, TT_ADDRESS_OF, ast->type, ast->location);
        node->address_of.operand = operand;
        return node;
    }
    case NODE_DEREF: {
        TtNode *operand = lower_expression(low, ast->deref.operand);
        TtNode *node = tt_new(low->tt_arena, TT_DEREF, ast->type, ast->location);
        node->deref.operand = operand;
        return node;
    }
    default:
        break;
    }
    return tt_new(low->tt_arena, TT_UNIT_LITERAL, &TYPE_ERROR_INSTANCE, ast->location);
}
