#include "_codegen.h"

// ── Per-node expression emitters ───────────────────────────────────────

/** Emit a comma-separated list of expressions from @p nodes into a single string. */
static const char *join_expressions(CodeGenerator *generator, TtNode **nodes, int32_t count) {
    const char *result = "";
    for (int32_t i = 0; i < count; i++) {
        const char *elem = codegen_emit_expression(generator, nodes[i]);
        result = (i == 0) ? elem : arena_sprintf(generator->arena, "%s, %s", result, elem);
    }
    return result;
}

static const char *emit_bool_literal(const TtNode *node) {
    return node->bool_literal.value ? "true" : "false";
}

static const char *emit_int_literal(CodeGenerator *generator, const TtNode *node) {
    TypeKind kind = node->int_literal.int_kind;
    uint64_t value = node->int_literal.value;
    switch (kind) {
    case TYPE_I8:
    case TYPE_I16:
    case TYPE_I32:
        return arena_sprintf(generator->arena, "%lld", (long long)(int64_t)value);
    case TYPE_I64:
    case TYPE_I128:
    case TYPE_ISIZE:
        // Avoid C integer overflow: 9223372036854775808LL exceeds LLONG_MAX.
        if (value == (uint64_t)1 << 63) {
            return "(-9223372036854775807LL - 1)";
        }
        return arena_sprintf(generator->arena, "%lldLL", (long long)(int64_t)value);
    case TYPE_U8:
    case TYPE_U16:
    case TYPE_U32:
        return arena_sprintf(generator->arena, "%lluU", (unsigned long long)value);
    case TYPE_U64:
    case TYPE_U128:
    case TYPE_USIZE:
        return arena_sprintf(generator->arena, "%lluULL", (unsigned long long)value);
    default:
        return arena_sprintf(generator->arena, "%lld", (long long)(int64_t)value);
    }
}

static const char *emit_float_literal(CodeGenerator *generator, const TtNode *node) {
    if (node->float_literal.float_kind == TYPE_F32) {
        return codegen_format_float32(generator, node->float_literal.value);
    }
    return codegen_format_float64(generator, node->float_literal.value);
}

static const char *emit_string_literal(CodeGenerator *generator, const TtNode *node) {
    const char *escaped = codegen_c_string_escape(generator, node->string_literal.value);
    return arena_sprintf(generator->arena, "rsg_string_literal(\"%s\")", escaped);
}

static const char *emit_unary_expression(CodeGenerator *generator, const TtNode *node) {
    const char *operand = codegen_emit_expression(generator, node->unary.operand);
    if (node->unary.op == TOKEN_BANG) {
        return arena_sprintf(generator->arena, "(!%s)", operand);
    }
    if (node->unary.op == TOKEN_MINUS) {
        return arena_sprintf(generator->arena, "(-%s)", operand);
    }
    return operand;
}

static const char *emit_binary_expression(CodeGenerator *generator, const TtNode *node) {
    const char *left = codegen_emit_expression(generator, node->binary.left);
    const char *right = codegen_emit_expression(generator, node->binary.right);
    const char *bin_op = token_kind_string(node->binary.op);
    return arena_sprintf(generator->arena, "(%s %s %s)", left, bin_op, right);
}

/** Return true if the callee is the rsg_assert runtime function. */
static bool is_rsg_assert_call(const TtNode *node) {
    if (node->call.callee->kind != TT_VARIABLE_REFERENCE) {
        return false;
    }
    return strcmp(tt_symbol_name(node->call.callee->variable_reference.symbol), "rsg_assert") == 0;
}

/**
 * Emit rsg_assert(cond, msg, file, line).
 * Lowering has already expanded assert() into rsg_assert() with four args.
 * The message (arg 1) is either a string literal (emit as C string), a unit
 * literal (emit NULL), or a string expression (emit .data).
 * The file (arg 2) is always a string literal emitted as a C string.
 */
static const char *emit_rsg_assert_call(CodeGenerator *generator, const TtNode *node) {
    const char *condition = codegen_emit_expression(generator, node->call.arguments[0]);

    const TtNode *msg_node = node->call.arguments[1];
    const char *message;
    if (msg_node->kind == TT_UNIT_LITERAL) {
        message = "NULL";
    } else if (msg_node->kind == TT_STRING_LITERAL) {
        message = arena_sprintf(generator->arena, "\"%s\"",
                                codegen_c_string_escape(generator, msg_node->string_literal.value));
    } else {
        const char *msg_expr = codegen_emit_expression(generator, msg_node);
        message = arena_sprintf(generator->arena, "%s.data", msg_expr);
    }

    const TtNode *file_node = node->call.arguments[2];
    const char *file =
        arena_sprintf(generator->arena, "\"%s\"",
                      codegen_c_string_escape(generator, file_node->string_literal.value));

    const char *line = codegen_emit_expression(generator, node->call.arguments[3]);

    return arena_sprintf(generator->arena, "rsg_assert(%s, %s, %s, %s)", condition, message, file,
                         line);
}

static const char *emit_call_expression(CodeGenerator *generator, const TtNode *node) {
    if (is_rsg_assert_call(node)) {
        return emit_rsg_assert_call(generator, node);
    }
    const char *callee;
    if (node->call.callee->kind == TT_VARIABLE_REFERENCE) {
        callee = node->call.callee->variable_reference.symbol->mangled_name;
    } else {
        callee = codegen_emit_expression(generator, node->call.callee);
    }
    const char *argument_list =
        join_expressions(generator, node->call.arguments, BUFFER_LENGTH(node->call.arguments));
    return arena_sprintf(generator->arena, "%s(%s)", callee, argument_list);
}

/** Return true if the node is a pure expression (no side effects). */
static bool is_pure_expression(const TtNode *node) {
    if (node == NULL) {
        return false;
    }
    switch (node->kind) {
    case TT_BOOL_LITERAL:
    case TT_INT_LITERAL:
    case TT_FLOAT_LITERAL:
    case TT_CHAR_LITERAL:
    case TT_STRING_LITERAL:
    case TT_UNIT_LITERAL:
    case TT_VARIABLE_REFERENCE:
        return true;
    case TT_UNARY:
        return is_pure_expression(node->unary.operand);
    case TT_BINARY:
        return is_pure_expression(node->binary.left) && is_pure_expression(node->binary.right);
    default:
        return false;
    }
}

/** Return the simple result expression from a branch body, or NULL. */
static const TtNode *simple_branch_result(const TtNode *body) {
    if (body == NULL) {
        return NULL;
    }
    if (body->kind == TT_BLOCK) {
        if (BUFFER_LENGTH(body->block.statements) != 0 || body->block.result == NULL) {
            return NULL;
        }
        return body->block.result;
    }
    return body;
}

/** Return true if a TT_IF can be emitted as a C ternary. */
static bool is_ternary_candidate(const TtNode *node) {
    if (node->kind != TT_IF || node->if_expression.else_body == NULL) {
        return false;
    }
    const TtNode *then_result = simple_branch_result(node->if_expression.then_body);
    if (then_result == NULL || !is_pure_expression(then_result)) {
        return false;
    }
    const TtNode *else_body = node->if_expression.else_body;
    if (else_body->kind == TT_IF) {
        return false;
    }
    const TtNode *else_result = simple_branch_result(else_body);
    return else_result != NULL && is_pure_expression(else_result);
}

static const char *emit_if_expression(CodeGenerator *generator, const TtNode *node) {
    // Emit as C ternary when both branches are pure single-expression results
    if (is_ternary_candidate(node)) {
        const char *condition = codegen_emit_expression(generator, node->if_expression.condition);
        const TtNode *then_result = simple_branch_result(node->if_expression.then_body);
        const TtNode *else_result = simple_branch_result(node->if_expression.else_body);
        const char *then_value = codegen_emit_expression(generator, then_result);
        const char *else_value = codegen_emit_expression(generator, else_result);
        return arena_sprintf(generator->arena, "(%s ? %s : %s)", condition, then_value, else_value);
    }

    const Type *type = node->type;
    if (type == NULL || type->kind == TYPE_UNIT) {
        codegen_emit_if(generator, node, NULL, false);
        return "(void)0";
    }
    const char *temporary = codegen_next_temporary(generator);
    codegen_emit_line(generator, "%s %s;", codegen_c_type_for(generator, type), temporary);
    codegen_emit_if(generator, node, temporary, false);
    return temporary;
}

static const char *emit_block_expression(CodeGenerator *generator, const TtNode *node) {
    codegen_emit_block_statements(generator, node);
    if (node->block.result != NULL) {
        return codegen_emit_expression(generator, node->block.result);
    }
    return "(void)0";
}

static const char *emit_array_literal_expression(CodeGenerator *generator, const TtNode *node) {
    const char *tname = codegen_c_type_for(generator, node->type);
    const char *elements = join_expressions(generator, node->array_literal.elements,
                                            BUFFER_LENGTH(node->array_literal.elements));
    return arena_sprintf(generator->arena, "(%s){ ._data = { %s } }", tname, elements);
}

static const char *emit_tuple_literal_expression(CodeGenerator *generator, const TtNode *node) {
    const char *tname = codegen_c_type_for(generator, node->type);
    const char *result = arena_sprintf(generator->arena, "(%s){ ", tname);
    for (int32_t i = 0; i < BUFFER_LENGTH(node->tuple_literal.elements); i++) {
        if (i > 0) {
            result = arena_sprintf(generator->arena, "%s, ", result);
        }
        const char *elem = codegen_emit_expression(generator, node->tuple_literal.elements[i]);
        result = arena_sprintf(generator->arena, "%s._%d = %s", result, i, elem);
    }
    return arena_sprintf(generator->arena, "%s }", result);
}

static const char *emit_index_expression(CodeGenerator *generator, const TtNode *node) {
    const char *object = codegen_emit_expression(generator, node->index_access.object);
    const char *index = codegen_emit_expression(generator, node->index_access.index);
    return arena_sprintf(generator->arena, "%s._data[%s]", object, index);
}

static const char *emit_tuple_index_expression(CodeGenerator *generator, const TtNode *node) {
    const char *object = codegen_emit_expression(generator, node->tuple_index.object);
    return arena_sprintf(generator->arena, "%s._%d", object, node->tuple_index.element_index);
}

static const char *emit_type_conversion_expression(CodeGenerator *generator, const TtNode *node) {
    return codegen_emit_expression(generator, node->type_conversion.operand);
}

static const char *emit_module_access_expression(CodeGenerator *generator, const TtNode *node) {
    const char *object = codegen_emit_expression(generator, node->module_access.object);
    return arena_sprintf(generator->arena, "%s._%s", object, node->module_access.member);
}

static const char *emit_struct_literal_expression(CodeGenerator *generator, const TtNode *node) {
    const char *tname = codegen_c_type_for(generator, node->type);
    const char *result = arena_sprintf(generator->arena, "(%s){ ", tname);
    for (int32_t i = 0; i < BUFFER_LENGTH(node->struct_literal.field_names); i++) {
        if (i > 0) {
            result = arena_sprintf(generator->arena, "%s, ", result);
        }
        const char *value =
            codegen_emit_expression(generator, node->struct_literal.field_values[i]);
        result = arena_sprintf(generator->arena, "%s.%s = %s", result,
                               node->struct_literal.field_names[i], value);
    }
    return arena_sprintf(generator->arena, "%s }", result);
}

static const char *emit_struct_field_access_expression(CodeGenerator *generator,
                                                       const TtNode *node) {
    const char *object = codegen_emit_expression(generator, node->struct_field_access.object);
    if (node->struct_field_access.via_pointer) {
        return arena_sprintf(generator->arena, "%s->%s", object, node->struct_field_access.field);
    }
    return arena_sprintf(generator->arena, "%s.%s", object, node->struct_field_access.field);
}

static const char *emit_method_call_expression(CodeGenerator *generator, const TtNode *node) {
    const char *receiver = codegen_emit_expression(generator, node->method_call.receiver);
    int32_t arg_count = BUFFER_LENGTH(node->method_call.arguments);
    if (arg_count > 0) {
        const char *args = join_expressions(generator, node->method_call.arguments, arg_count);
        return arena_sprintf(generator->arena, "%s(&(%s), %s)", node->method_call.mangled_name,
                             receiver, args);
    }
    return arena_sprintf(generator->arena, "%s(&(%s))", node->method_call.mangled_name, receiver);
}

static const char *emit_heap_alloc_expression(CodeGenerator *generator, const TtNode *node) {
    const char *pointee_type = codegen_c_type_for(generator, node->type->pointer.pointee);
    const char *tmp = codegen_next_temporary(generator);
    const char *inner = codegen_emit_expression(generator, node->heap_alloc.operand);
    codegen_emit_line(generator, "%s *%s = rsg_heap_alloc(sizeof(%s));", pointee_type, tmp,
                      pointee_type);
    codegen_emit_line(generator, "*%s = %s;", tmp, inner);
    return tmp;
}

static const char *emit_address_of_expression(CodeGenerator *generator, const TtNode *node) {
    const char *operand = codegen_emit_expression(generator, node->address_of.operand);
    return arena_sprintf(generator->arena, "&(%s)", operand);
}

static const char *emit_deref_expression(CodeGenerator *generator, const TtNode *node) {
    const char *operand = codegen_emit_expression(generator, node->deref.operand);
    return arena_sprintf(generator->arena, "(*%s)", operand);
}

/** Emit a match expression as a temp variable with an if-else chain. */
/** Emit a single guarded match arm (used inside do { } while(0) block). */
static void emit_match_arm_guarded(CodeGenerator *generator, const TtNode *condition,
                                   const TtNode *guard, const TtNode *body, const TtNode *bindings,
                                   const char *result) {
    if (condition != NULL) {
        const char *cond_str = codegen_emit_expression(generator, condition);
        const char *wrapped = cond_str;
        if (cond_str[0] != '(') {
            wrapped = arena_sprintf(generator->arena, "(%s)", cond_str);
        }
        codegen_emit_line(generator, "if %s {", wrapped);
    } else {
        codegen_emit_line(generator, "{");
    }
    generator->indent++;

    if (bindings != NULL && bindings->kind == TT_BLOCK) {
        codegen_emit_block_statements(generator, bindings);
    }

    if (guard != NULL) {
        const char *guard_str = codegen_emit_expression(generator, guard);
        const char *gwrapped = guard_str;
        if (guard_str[0] != '(') {
            gwrapped = arena_sprintf(generator->arena, "(%s)", guard_str);
        }
        codegen_emit_line(generator, "if %s {", gwrapped);
        generator->indent++;
    }

    if (result != NULL) {
        const char *value = codegen_emit_expression(generator, body);
        codegen_emit_line(generator, "%s = %s;", result, value);
    } else {
        codegen_emit_statement(generator, body);
    }
    codegen_emit_line(generator, "break;");

    if (guard != NULL) {
        generator->indent--;
        codegen_emit_line(generator, "}");
    }
    generator->indent--;
    codegen_emit_line(generator, "}");
}

/** Emit a single simple match arm (part of if-else chain, no guards). */
static void emit_match_arm_simple(CodeGenerator *generator, int32_t arm_index,
                                  const TtNode *condition, const TtNode *body,
                                  const TtNode *bindings, const char *result) {
    if (condition != NULL) {
        const char *cond_str = codegen_emit_expression(generator, condition);
        const char *wrapped = cond_str;
        if (cond_str[0] != '(') {
            wrapped = arena_sprintf(generator->arena, "(%s)", cond_str);
        }
        if (arm_index == 0) {
            codegen_emit_line(generator, "if %s {", wrapped);
        } else {
            codegen_emit_indent(generator);
            fprintf(generator->output, "} else if %s {\n", wrapped);
        }
    } else {
        if (arm_index == 0) {
            codegen_emit_line(generator, "{");
        } else {
            codegen_emit_line(generator, "} else {");
        }
    }
    generator->indent++;

    if (bindings != NULL && bindings->kind == TT_BLOCK) {
        codegen_emit_block_statements(generator, bindings);
    }

    if (result != NULL) {
        const char *value = codegen_emit_expression(generator, body);
        codegen_emit_line(generator, "%s = %s;", result, value);
    } else {
        codegen_emit_statement(generator, body);
    }
    generator->indent--;
}

static const char *emit_match_expression(CodeGenerator *generator, const TtNode *node) {
    const Type *type = node->type;

    codegen_emit_statement(generator, node->match_expr.operand);

    const char *result = NULL;
    if (type != NULL && type->kind != TYPE_UNIT) {
        result = codegen_next_temporary(generator);
        codegen_emit_line(generator, "%s %s;", codegen_c_type_for(generator, type), result);
    }

    int32_t arm_count = BUFFER_LENGTH(node->match_expr.arm_conditions);
    bool has_any_guard = false;
    for (int32_t i = 0; i < arm_count; i++) {
        if (node->match_expr.arm_guards[i] != NULL) {
            has_any_guard = true;
            break;
        }
    }

    if (has_any_guard) {
        codegen_emit_line(generator, "do {");
        generator->indent++;
    }

    for (int32_t i = 0; i < arm_count; i++) {
        if (has_any_guard) {
            emit_match_arm_guarded(generator, node->match_expr.arm_conditions[i],
                                   node->match_expr.arm_guards[i], node->match_expr.arm_bodies[i],
                                   node->match_expr.arm_bindings[i], result);
        } else {
            emit_match_arm_simple(generator, i, node->match_expr.arm_conditions[i],
                                  node->match_expr.arm_bodies[i], node->match_expr.arm_bindings[i],
                                  result);
        }
    }

    if (has_any_guard) {
        generator->indent--;
        codegen_emit_line(generator, "} while (0);");
    } else {
        codegen_emit_line(generator, "}");
    }

    return result != NULL ? result : "(void)0";
}

// ── Expression dispatch ────────────────────────────────────────────────

const char *codegen_emit_expression(CodeGenerator *generator, const TtNode *node) {
    if (node == NULL) {
        return "0";
    }
    switch (node->kind) {
    case TT_BOOL_LITERAL:
        return emit_bool_literal(node);
    case TT_INT_LITERAL:
        return emit_int_literal(generator, node);
    case TT_FLOAT_LITERAL:
        return emit_float_literal(generator, node);
    case TT_CHAR_LITERAL:
        return codegen_c_char_escape(generator, node->char_literal.value);
    case TT_STRING_LITERAL:
        return emit_string_literal(generator, node);
    case TT_UNIT_LITERAL:
        return "(void)0";
    case TT_VARIABLE_REFERENCE:
        return node->variable_reference.symbol->mangled_name;
    case TT_UNARY:
        return emit_unary_expression(generator, node);
    case TT_BINARY:
        return emit_binary_expression(generator, node);
    case TT_CALL:
        return emit_call_expression(generator, node);
    case TT_MODULE_ACCESS:
        return emit_module_access_expression(generator, node);
    case TT_INDEX:
        return emit_index_expression(generator, node);
    case TT_TUPLE_INDEX:
        return emit_tuple_index_expression(generator, node);
    case TT_IF:
        return emit_if_expression(generator, node);
    case TT_BLOCK:
        return emit_block_expression(generator, node);
    case TT_ARRAY_LITERAL:
        return emit_array_literal_expression(generator, node);
    case TT_TUPLE_LITERAL:
        return emit_tuple_literal_expression(generator, node);
    case TT_TYPE_CONVERSION:
        return emit_type_conversion_expression(generator, node);
    case TT_STRUCT_LITERAL:
        return emit_struct_literal_expression(generator, node);
    case TT_STRUCT_FIELD_ACCESS:
        return emit_struct_field_access_expression(generator, node);
    case TT_METHOD_CALL:
        return emit_method_call_expression(generator, node);
    case TT_HEAP_ALLOC:
        return emit_heap_alloc_expression(generator, node);
    case TT_ADDRESS_OF:
        return emit_address_of_expression(generator, node);
    case TT_DEREF:
        return emit_deref_expression(generator, node);
    case TT_MATCH:
        return emit_match_expression(generator, node);
    default:
        return "0";
    }
}
