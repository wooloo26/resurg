#include "_codegen.h"

// ── Ternary optimisation helpers ───────────────────────────────────────

/**
 * Return the simple result expression from a branch body, or NULL if the
 * body contains statements that prevent ternary emission.
 */
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
    // Direct expression (not a block)
    return body;
}

/**
 * Return true if the node evaluates to a C expression string without
 * emitting any side-effecting lines.
 */
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

/**
 * Return true if the if-expression has both branches, and both produce
 * a trivial (pure) result — safe to emit as `cond ? a : b`.
 */
static bool is_simple_ternary(const TtNode *node) {
    if (node->kind != TT_IF || node->if_expression.else_body == NULL) {
        return false;
    }
    const TtNode *then_result = simple_branch_result(node->if_expression.then_body);
    if (then_result == NULL || !is_pure_expression(then_result)) {
        return false;
    }
    const TtNode *else_body = node->if_expression.else_body;
    if (else_body->kind == TT_IF) {
        return false; // else-if chains stay as statements
    }
    const TtNode *else_result = simple_branch_result(else_body);
    return else_result != NULL && is_pure_expression(else_result);
}

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
    // Fold negation into integer/float literals to avoid overflow issues
    if (node->unary.op == TOKEN_MINUS && node->unary.operand->kind == TT_INT_LITERAL) {
        const TtNode *literal = node->unary.operand;
        TypeKind kind = literal->int_literal.int_kind;
        uint64_t value = literal->int_literal.value;
        switch (kind) {
        case TYPE_I8:
        case TYPE_I16:
            return arena_sprintf(generator->arena, "%lld", -(long long)(int64_t)value);
        case TYPE_I32: {
            int64_t negated = -(int64_t)value;
            if (negated == (int64_t)(-2147483647 - 1)) {
                return "(-2147483647 - 1)";
            }
            return arena_sprintf(generator->arena, "%lld", (long long)negated);
        }
        case TYPE_I64:
        case TYPE_I128:
        case TYPE_ISIZE:
            if (value == (uint64_t)9223372036854775808ULL) {
                return "(-9223372036854775807LL - 1)";
            }
            return arena_sprintf(generator->arena, "%lldLL", -(long long)(int64_t)value);
        default:
            break;
        }
    }
    if (node->unary.op == TOKEN_MINUS && node->unary.operand->kind == TT_FLOAT_LITERAL) {
        const TtNode *literal = node->unary.operand;
        if (literal->float_literal.float_kind == TYPE_F32) {
            return codegen_format_float32(generator, -literal->float_literal.value);
        }
        return codegen_format_float64(generator, -literal->float_literal.value);
    }
    const char *operand = codegen_emit_expression(generator, node->unary.operand);
    if (node->unary.op == TOKEN_BANG) {
        return arena_sprintf(generator->arena, "(!%s)", operand);
    }
    if (node->unary.op == TOKEN_MINUS) {
        return arena_sprintf(generator->arena, "(-%s)", operand);
    }
    return operand;
}

/** Attempt to constant-fold a binary op on two i32 literals. Returns NULL if not foldable. */
static const char *fold_i32_binary(CodeGenerator *generator, const TtNode *node) {
    const TtNode *left_operand = node->binary.left;
    const TtNode *right_operand = node->binary.right;

    bool both_i32 = left_operand->kind == TT_INT_LITERAL && right_operand->kind == TT_INT_LITERAL &&
                    left_operand->int_literal.int_kind == TYPE_I32 &&
                    right_operand->int_literal.int_kind == TYPE_I32;
    if (!both_i32) {
        return NULL;
    }

    int64_t left_value = (int64_t)left_operand->int_literal.value;
    int64_t right_value = (int64_t)right_operand->int_literal.value;
    int64_t result;
    bool folded = true;
    switch (node->binary.op) {
    case TOKEN_PLUS:
        result = left_value + right_value;
        break;
    case TOKEN_MINUS:
        result = left_value - right_value;
        break;
    case TOKEN_STAR:
        result = left_value * right_value;
        break;
    case TOKEN_SLASH:
        folded = (right_value != 0);
        if (folded) {
            result = left_value / right_value;
        }
        break;
    case TOKEN_PERCENT:
        folded = (right_value != 0);
        if (folded) {
            result = left_value % right_value;
        }
        break;
    default:
        folded = false;
        break;
    }
    if (!folded) {
        return NULL;
    }
    if (result == (int64_t)(-2147483647 - 1)) {
        return "(-2147483647 - 1)";
    }
    return arena_sprintf(generator->arena, "%lld", (long long)result);
}

/** Emit array equality/inequality via memcmp on ._data. */
static const char *emit_array_comparison(CodeGenerator *generator, const Type *type, TokenKind op,
                                         const char *left, const char *right) {
    const char *left_tmp = codegen_next_temporary(generator);
    const char *right_tmp = codegen_next_temporary(generator);
    const char *tname = codegen_c_type_for(generator, type);
    codegen_emit_line(generator, "%s %s = %s;", tname, left_tmp, left);
    codegen_emit_line(generator, "%s %s = %s;", tname, right_tmp, right);
    const char *cmp = (op == TOKEN_EQUAL_EQUAL) ? "==" : "!=";
    return arena_sprintf(generator->arena, "(memcmp(%s._data, %s._data, sizeof(%s._data)) %s 0)",
                         left_tmp, right_tmp, left_tmp, cmp);
}

/** Emit tuple equality/inequality via element-wise comparison. */
static const char *emit_tuple_comparison(CodeGenerator *generator, const Type *type, TokenKind op,
                                         const char *left, const char *right) {
    bool is_equal = (op == TOKEN_EQUAL_EQUAL);
    const char *left_tmp = codegen_next_temporary(generator);
    const char *right_tmp = codegen_next_temporary(generator);
    const char *tname = codegen_c_type_for(generator, type);
    codegen_emit_line(generator, "%s %s = %s;", tname, left_tmp, left);
    codegen_emit_line(generator, "%s %s = %s;", tname, right_tmp, right);
    const char *join = is_equal ? " && " : " || ";
    const char *cmp = is_equal ? "==" : "!=";
    const char *result = "";
    for (int32_t i = 0; i < type->tuple.count; i++) {
        const char *l = arena_sprintf(generator->arena, "%s._%d", left_tmp, i);
        const char *r = arena_sprintf(generator->arena, "%s._%d", right_tmp, i);
        const char *part;
        if (type->tuple.elements[i]->kind == TYPE_STRING) {
            if (is_equal) {
                part = arena_sprintf(generator->arena, "rsg_string_equal(%s, %s)", l, r);
            } else {
                part = arena_sprintf(generator->arena, "(!rsg_string_equal(%s, %s))", l, r);
            }
        } else {
            part = arena_sprintf(generator->arena, "(%s %s %s)", l, cmp, r);
        }
        if (i == 0) {
            result = part;
        } else {
            result = arena_sprintf(generator->arena, "%s%s%s", result, join, part);
        }
    }
    return arena_sprintf(generator->arena, "(%s)", result);
}

static const char *emit_binary_expression(CodeGenerator *generator, const TtNode *node) {
    const char *folded = fold_i32_binary(generator, node);
    if (folded != NULL) {
        return folded;
    }

    const TtNode *left_operand = node->binary.left;
    const TtNode *right_operand = node->binary.right;
    const char *left = codegen_emit_expression(generator, left_operand);
    const char *right = codegen_emit_expression(generator, right_operand);

    const Type *left_type = left_operand->type;
    TokenKind op = node->binary.op;

    // String equality/inequality
    if (left_type != NULL && left_type->kind == TYPE_STRING) {
        if (op == TOKEN_EQUAL_EQUAL) {
            return arena_sprintf(generator->arena, "rsg_string_equal(%s, %s)", left, right);
        }
        if (op == TOKEN_BANG_EQUAL) {
            return arena_sprintf(generator->arena, "(!rsg_string_equal(%s, %s))", left, right);
        }
    }

    // Array equality/inequality
    if (left_type != NULL && left_type->kind == TYPE_ARRAY &&
        (op == TOKEN_EQUAL_EQUAL || op == TOKEN_BANG_EQUAL)) {
        return emit_array_comparison(generator, left_type, op, left, right);
    }

    // Tuple equality/inequality
    if (left_type != NULL && left_type->kind == TYPE_TUPLE &&
        (op == TOKEN_EQUAL_EQUAL || op == TOKEN_BANG_EQUAL)) {
        return emit_tuple_comparison(generator, left_type, op, left, right);
    }

    const char *bin_op = codegen_c_binary_operator(op);
    return arena_sprintf(generator->arena, "(%s %s %s)", left, bin_op, right);
}

/** Return true if the callee is an assert builtin. */
static bool is_assert_call(const TtNode *node) {
    if (node->call.callee->kind != TT_VARIABLE_REFERENCE) {
        return false;
    }
    return strcmp(tt_symbol_name(node->call.callee->variable_reference.symbol), "assert") == 0;
}

static const char *emit_assert_call(CodeGenerator *generator, const TtNode *node) {
    int32_t argument_count = BUFFER_LENGTH(node->call.arguments);
    const char *condition = "0";
    if (argument_count > 0) {
        condition = codegen_emit_expression(generator, node->call.arguments[0]);
    }
    const char *message = "NULL";
    if (argument_count > 1) {
        const TtNode *message_node = node->call.arguments[1];
        if (message_node->kind == TT_STRING_LITERAL) {
            message = arena_sprintf(
                generator->arena, "\"%s\"",
                codegen_c_string_escape(generator, message_node->string_literal.value));
        } else {
            const char *message_expression = codegen_emit_expression(generator, message_node);
            message = arena_sprintf(generator->arena, "%s.data", message_expression);
        }
    }
    return arena_sprintf(generator->arena, "rsg_assert(%s, %s, _rsg_file, %d)", condition, message,
                         node->location.line);
}

/** Return true if the callee is a runtime builtin (rsg_* name). */
static bool is_runtime_builtin(const TtNode *callee) {
    if (callee->kind != TT_VARIABLE_REFERENCE) {
        return false;
    }
    const char *name = tt_symbol_name(callee->variable_reference.symbol);
    return strncmp(name, "rsg_", 4) == 0;
}

static const char *emit_call_expression(CodeGenerator *generator, const TtNode *node) {
    if (is_assert_call(node)) {
        return emit_assert_call(generator, node);
    }
    const char *callee;
    if (node->call.callee->kind == TT_VARIABLE_REFERENCE) {
        const char *name = tt_symbol_name(node->call.callee->variable_reference.symbol);
        if (is_runtime_builtin(node->call.callee)) {
            callee = name; // runtime builtins keep their name
        } else {
            callee = codegen_mangle_function_name(generator, name);
        }
    } else {
        callee = codegen_emit_expression(generator, node->call.callee);
    }
    const char *argument_list =
        join_expressions(generator, node->call.arguments, BUFFER_LENGTH(node->call.arguments));
    return arena_sprintf(generator->arena, "%s(%s)", callee, argument_list);
}

static const char *emit_if_expression(CodeGenerator *generator, const TtNode *node) {
    const Type *type = node->type;
    if (type == NULL || type->kind == TYPE_UNIT) {
        codegen_emit_if(generator, node, NULL, false);
        return "(void)0";
    }
    if (is_simple_ternary(node)) {
        const char *condition = codegen_emit_expression(generator, node->if_expression.condition);
        const TtNode *then_result = simple_branch_result(node->if_expression.then_body);
        const TtNode *else_result = simple_branch_result(node->if_expression.else_body);
        const char *then_value = codegen_emit_expression(generator, then_result);
        const char *else_value = codegen_emit_expression(generator, else_result);
        return arena_sprintf(generator->arena, "(%s ? %s : %s)", condition, then_value, else_value);
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
        return codegen_variable_lookup(generator, tt_symbol_name(node->variable_reference.symbol));
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
    default:
        return "0";
    }
}
