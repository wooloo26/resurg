#include "codegen/internal.h"

// ── Ternary optimisation helpers ───────────────────────────────────────

/**
 * Return the simple result expression from a branch body, or NULL if the
 * body contains statements that prevent ternary emission.
 */
static const ASTNode *simple_branch_result(const ASTNode *body) {
    if (body == NULL) {
        return NULL;
    }
    if (body->kind == NODE_BLOCK) {
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
static bool is_pure_expression(const ASTNode *node) {
    if (node == NULL) {
        return false;
    }
    switch (node->kind) {
    case NODE_LITERAL:
    case NODE_IDENTIFIER:
        return true;
    case NODE_UNARY:
        return is_pure_expression(node->unary.operand);
    case NODE_BINARY:
        return is_pure_expression(node->binary.left) && is_pure_expression(node->binary.right);
    default:
        return false;
    }
}

/**
 * Return true if the if-expression has both branches, and both produce
 * a trivial (pure) result — safe to emit as `cond ? a : b`.
 */
static bool is_simple_ternary(const ASTNode *node) {
    if (node->kind != NODE_IF || node->if_expression.else_body == NULL) {
        return false;
    }
    const ASTNode *then_result = simple_branch_result(node->if_expression.then_body);
    if (then_result == NULL || !is_pure_expression(then_result)) {
        return false;
    }
    const ASTNode *else_body = node->if_expression.else_body;
    if (else_body->kind == NODE_IF) {
        return false; // else-if chains stay as statements
    }
    const ASTNode *else_result = simple_branch_result(else_body);
    return else_result != NULL && is_pure_expression(else_result);
}

// ── String conversion for interpolation ────────────────────────────────

/**
 * Convert an expression node to an RsgString value for interpolation,
 * wrapping non-string types with the appropriate rsg_string_from_* call.
 */
static const char *string_convert_expression(CodeGenerator *generator, const ASTNode *node) {
    const char *value = codegen_emit_expression(generator, node);
    const Type *type = node->type;
    if (type == NULL) {
        return value;
    }
    switch (type->kind) {
    case TYPE_STRING:
        return value;
    case TYPE_BOOL:
        return arena_sprintf(generator->arena, "rsg_string_from_bool(%s)", value);
    case TYPE_I8:
    case TYPE_I16:
    case TYPE_I32:
        return arena_sprintf(generator->arena, "rsg_string_from_i32((int32_t)(%s))", value);
    case TYPE_I64:
    case TYPE_I128:
    case TYPE_ISIZE:
        return arena_sprintf(generator->arena, "rsg_string_from_i64((int64_t)(%s))", value);
    case TYPE_U8:
    case TYPE_U16:
    case TYPE_U32:
        return arena_sprintf(generator->arena, "rsg_string_from_u32((uint32_t)(%s))", value);
    case TYPE_U64:
    case TYPE_U128:
    case TYPE_USIZE:
        return arena_sprintf(generator->arena, "rsg_string_from_u64((uint64_t)(%s))", value);
    case TYPE_F32:
        return arena_sprintf(generator->arena, "rsg_string_from_f32(%s)", value);
    case TYPE_F64:
        return arena_sprintf(generator->arena, "rsg_string_from_f64(%s)", value);
    case TYPE_CHAR:
        return arena_sprintf(generator->arena, "rsg_string_from_char(%s)", value);
    default:
        return arena_sprintf(generator->arena, "rsg_string_from_i32((int32_t)(%s))", value);
    }
}

// ── Per-node expression emitters ───────────────────────────────────────

static const char *emit_literal_expression(CodeGenerator *generator, const ASTNode *node) {
    switch (node->literal.kind) {
    case LITERAL_BOOL:
        return node->literal.boolean_value ? "true" : "false";
    case LITERAL_I8:
    case LITERAL_I16:
    case LITERAL_I32:
        return arena_sprintf(generator->arena, "%lld", (long long)(int64_t)node->literal.integer_value);
    case LITERAL_I64:
    case LITERAL_I128:
    case LITERAL_ISIZE:
        return arena_sprintf(generator->arena, "%lldLL", (long long)(int64_t)node->literal.integer_value);
    case LITERAL_U8:
    case LITERAL_U16:
    case LITERAL_U32:
        return arena_sprintf(generator->arena, "%lluU", (unsigned long long)node->literal.integer_value);
    case LITERAL_U64:
    case LITERAL_U128:
    case LITERAL_USIZE:
        return arena_sprintf(generator->arena, "%lluULL", (unsigned long long)node->literal.integer_value);
    case LITERAL_F32:
        return codegen_format_float32(generator, node->literal.float64_value);
    case LITERAL_F64:
        return codegen_format_float64(generator, node->literal.float64_value);
    case LITERAL_CHAR: {
        char c = node->literal.char_value;
        switch (c) {
        case '\n':
            return "'\\n'";
        case '\t':
            return "'\\t'";
        case '\\':
            return "'\\\\'";
        case '\'':
            return "'\\\''";
        case '\0':
            return "'\\0'";
        default:
            return arena_sprintf(generator->arena, "'%c'", c);
        }
    }
    case LITERAL_STRING: {
        const char *escaped = codegen_c_string_escape(generator, node->literal.string_value);
        return arena_sprintf(generator->arena, "rsg_string_literal(\"%s\")", escaped);
    }
    case LITERAL_UNIT:
        return "(void)0";
    }
    return "0";
}

static const char *emit_unary_expression(CodeGenerator *generator, const ASTNode *node) {
    // Fold negation into integer/float literals to avoid overflow issues
    if (node->unary.op == TOKEN_MINUS && node->unary.operand->kind == NODE_LITERAL) {
        const ASTNode *literal = node->unary.operand;
        switch (literal->literal.kind) {
        case LITERAL_I8:
        case LITERAL_I16:
            return arena_sprintf(generator->arena, "%lld", -(long long)(int64_t)literal->literal.integer_value);
        case LITERAL_I32: {
            int64_t negated = -(int64_t)literal->literal.integer_value;
            if (negated == (int64_t)(-2147483647 - 1)) {
                return "(-2147483647 - 1)";
            }
            return arena_sprintf(generator->arena, "%lld", (long long)negated);
        }
        case LITERAL_I64:
        case LITERAL_I128:
        case LITERAL_ISIZE: {
            uint64_t value = literal->literal.integer_value;
            if (value == (uint64_t)9223372036854775808ULL) {
                return "(-9223372036854775807LL - 1)";
            }
            return arena_sprintf(generator->arena, "%lldLL", -(long long)(int64_t)value);
        }
        case LITERAL_F32:
            return codegen_format_float32(generator, -literal->literal.float64_value);
        case LITERAL_F64:
            return codegen_format_float64(generator, -literal->literal.float64_value);
        default:
            break;
        }
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
static const char *fold_i32_binary(CodeGenerator *generator, const ASTNode *node) {
    const ASTNode *left_operand = node->binary.left;
    const ASTNode *right_operand = node->binary.right;

    bool both_i32 = left_operand->kind == NODE_LITERAL && right_operand->kind == NODE_LITERAL &&
                    left_operand->literal.kind == LITERAL_I32 && right_operand->literal.kind == LITERAL_I32;
    if (!both_i32) {
        return NULL;
    }

    int64_t left_value = (int64_t)left_operand->literal.integer_value;
    int64_t right_value = (int64_t)right_operand->literal.integer_value;
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

static const char *emit_binary_expression(CodeGenerator *generator, const ASTNode *node) {
    const char *folded = fold_i32_binary(generator, node);
    if (folded != NULL) {
        return folded;
    }

    const ASTNode *left_operand = node->binary.left;
    const ASTNode *right_operand = node->binary.right;
    const char *left = codegen_emit_expression(generator, left_operand);
    const char *right = codegen_emit_expression(generator, right_operand);

    const Type *left_type = left_operand->type;

    // String equality/inequality
    if (left_type != NULL && left_type->kind == TYPE_STRING) {
        if (node->binary.op == TOKEN_EQUAL_EQUAL) {
            return arena_sprintf(generator->arena, "rsg_string_equal(%s, %s)", left, right);
        }
        if (node->binary.op == TOKEN_BANG_EQUAL) {
            return arena_sprintf(generator->arena, "(!rsg_string_equal(%s, %s))", left, right);
        }
    }

    // Array equality/inequality — memcmp on ._data
    if (left_type != NULL && left_type->kind == TYPE_ARRAY &&
        (node->binary.op == TOKEN_EQUAL_EQUAL || node->binary.op == TOKEN_BANG_EQUAL)) {
        const char *left_tmp = codegen_next_temporary(generator);
        const char *right_tmp = codegen_next_temporary(generator);
        const char *tname = codegen_c_type_for(generator, left_type);
        codegen_emit_line(generator, "%s %s = %s;", tname, left_tmp, left);
        codegen_emit_line(generator, "%s %s = %s;", tname, right_tmp, right);
        const char *cmp = (node->binary.op == TOKEN_EQUAL_EQUAL) ? "==" : "!=";
        return arena_sprintf(generator->arena, "(memcmp(%s._data, %s._data, sizeof(%s._data)) %s 0)", left_tmp,
                             right_tmp, left_tmp, cmp);
    }

    // Tuple equality/inequality — element-wise comparison
    if (left_type != NULL && left_type->kind == TYPE_TUPLE &&
        (node->binary.op == TOKEN_EQUAL_EQUAL || node->binary.op == TOKEN_BANG_EQUAL)) {
        bool is_equal = (node->binary.op == TOKEN_EQUAL_EQUAL);
        const char *left_tmp = codegen_next_temporary(generator);
        const char *right_tmp = codegen_next_temporary(generator);
        const char *tname = codegen_c_type_for(generator, left_type);
        codegen_emit_line(generator, "%s %s = %s;", tname, left_tmp, left);
        codegen_emit_line(generator, "%s %s = %s;", tname, right_tmp, right);
        const char *join = is_equal ? " && " : " || ";
        const char *cmp = is_equal ? "==" : "!=";
        const char *result = "";
        for (int32_t i = 0; i < left_type->tuple_count; i++) {
            const char *l = arena_sprintf(generator->arena, "%s._%d", left_tmp, i);
            const char *r = arena_sprintf(generator->arena, "%s._%d", right_tmp, i);
            const char *part;
            if (left_type->tuple_elements[i]->kind == TYPE_STRING) {
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

    const char *op = codegen_c_binary_operator(node->binary.op);
    return arena_sprintf(generator->arena, "(%s %s %s)", left, op, right);
}

static const char *emit_assert_call(CodeGenerator *generator, const ASTNode *node) {
    int32_t argument_count = BUFFER_LENGTH(node->call.arguments);
    const char *condition = argument_count > 0 ? codegen_emit_expression(generator, node->call.arguments[0]) : "0";
    const char *message = "NULL";
    if (argument_count > 1) {
        ASTNode *message_node = node->call.arguments[1];
        if (message_node->kind == NODE_LITERAL && message_node->literal.kind == LITERAL_STRING) {
            message = arena_sprintf(generator->arena, "\"%s\"",
                                    codegen_c_string_escape(generator, message_node->literal.string_value));
        } else {
            const char *message_expression = codegen_emit_expression(generator, message_node);
            message = arena_sprintf(generator->arena, "%s.data", message_expression);
        }
    }
    return arena_sprintf(generator->arena, "rsg_assert(%s, %s, _rsg_file, %d)", condition, message,
                         node->location.line);
}

static const char *emit_call_expression(CodeGenerator *generator, const ASTNode *node) {
    if (node->call.callee->kind == NODE_IDENTIFIER && strcmp(node->call.callee->identifier.name, "assert") == 0) {
        return emit_assert_call(generator, node);
    }
    const char *callee;
    if (node->call.callee->kind == NODE_IDENTIFIER) {
        callee = codegen_mangle_function_name(generator, node->call.callee->identifier.name);
    } else {
        callee = codegen_emit_expression(generator, node->call.callee);
    }
    const char *argument_list = "";
    for (int32_t i = 0; i < BUFFER_LENGTH(node->call.arguments); i++) {
        const char *argument = codegen_emit_expression(generator, node->call.arguments[i]);
        if (i == 0) {
            argument_list = argument;
        } else {
            argument_list = arena_sprintf(generator->arena, "%s, %s", argument_list, argument);
        }
    }
    return arena_sprintf(generator->arena, "%s(%s)", callee, argument_list);
}

static const char *emit_if_expression(CodeGenerator *generator, const ASTNode *node) {
    const Type *type = node->type;
    if (type == NULL || type->kind == TYPE_UNIT) {
        codegen_emit_if(generator, node, NULL, false);
        return "(void)0";
    }
    if (is_simple_ternary(node)) {
        const char *condition = codegen_emit_expression(generator, node->if_expression.condition);
        const ASTNode *then_result = simple_branch_result(node->if_expression.then_body);
        const ASTNode *else_result = simple_branch_result(node->if_expression.else_body);
        const char *then_value = codegen_emit_expression(generator, then_result);
        const char *else_value = codegen_emit_expression(generator, else_result);
        return arena_sprintf(generator->arena, "(%s ? %s : %s)", condition, then_value, else_value);
    }
    const char *temporary = codegen_next_temporary(generator);
    codegen_emit_line(generator, "%s %s;", codegen_c_type_for(generator, type), temporary);
    codegen_emit_if(generator, node, temporary, false);
    return temporary;
}

static const char *emit_block_expression(CodeGenerator *generator, const ASTNode *node) {
    for (int32_t i = 0; i < BUFFER_LENGTH(node->block.statements); i++) {
        codegen_emit_statement(generator, node->block.statements[i]);
    }
    if (node->block.result != NULL) {
        return codegen_emit_expression(generator, node->block.result);
    }
    return "(void)0";
}

static const char *emit_string_interpolation_expression(CodeGenerator *generator, const ASTNode *node) {
    int32_t part_count = BUFFER_LENGTH(node->string_interpolation.parts);
    const char **string_parts = NULL;
    for (int32_t i = 0; i < part_count; i++) {
        const ASTNode *part = node->string_interpolation.parts[i];
        if (part->kind == NODE_LITERAL && part->literal.kind == LITERAL_STRING) {
            const char *text = part->literal.string_value;
            if (text == NULL || text[0] == '\0') {
                continue;
            }
            BUFFER_PUSH(string_parts, arena_sprintf(generator->arena, "rsg_string_literal(\"%s\")",
                                                    codegen_c_string_escape(generator, text)));
        } else {
            BUFFER_PUSH(string_parts, string_convert_expression(generator, part));
        }
    }
    int32_t count = BUFFER_LENGTH(string_parts);

    if (count == 1) {
        const char *result = string_parts[0];
        BUFFER_FREE(string_parts);
        return result;
    }
    if (count == 2) {
        const char *result =
            arena_sprintf(generator->arena, "rsg_string_concat(%s, %s)", string_parts[0], string_parts[1]);
        BUFFER_FREE(string_parts);
        return result;
    }

    const char *builder = codegen_next_string_builder(generator);
    codegen_emit_line(generator, "RsgStringBuilder %s;", builder);
    codegen_emit_line(generator, "rsg_string_builder_init(&%s);", builder);
    for (int32_t i = 0; i < count; i++) {
        codegen_emit_line(generator, "rsg_string_builder_append_string(&%s, %s);", builder, string_parts[i]);
    }
    BUFFER_FREE(string_parts);
    const char *temporary = codegen_next_temporary(generator);
    codegen_emit_line(generator, "RsgString %s = rsg_string_builder_finish(&%s);", temporary, builder);
    return temporary;
}

static const char *emit_array_literal_expression(CodeGenerator *generator, const ASTNode *node) {
    const Type *type = node->type;
    const char *tname = codegen_c_type_for(generator, type);
    const char *result = arena_sprintf(generator->arena, "(%s){ ._data = { ", tname);
    for (int32_t i = 0; i < BUFFER_LENGTH(node->array_literal.elements); i++) {
        if (i > 0) {
            result = arena_sprintf(generator->arena, "%s, ", result);
        }
        const char *elem = codegen_emit_expression(generator, node->array_literal.elements[i]);
        result = arena_sprintf(generator->arena, "%s%s", result, elem);
    }
    return arena_sprintf(generator->arena, "%s } }", result);
}

static const char *emit_tuple_literal_expression(CodeGenerator *generator, const ASTNode *node) {
    const Type *type = node->type;
    const char *tname = codegen_c_type_for(generator, type);
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

static const char *emit_index_expression(CodeGenerator *generator, const ASTNode *node) {
    const char *object = codegen_emit_expression(generator, node->index_access.object);
    const char *index = codegen_emit_expression(generator, node->index_access.index);
    return arena_sprintf(generator->arena, "%s._data[%s]", object, index);
}

static const char *emit_type_conversion_expression(CodeGenerator *generator, const ASTNode *node) {
    return codegen_emit_expression(generator, node->type_conversion.operand);
}

static const char *emit_member_expression(CodeGenerator *generator, const ASTNode *node) {
    const char *object = codegen_emit_expression(generator, node->member.object);
    return arena_sprintf(generator->arena, "%s._%s", object, node->member.member);
}

// ── Expression dispatch ────────────────────────────────────────────────

const char *codegen_emit_expression(CodeGenerator *generator, const ASTNode *node) {
    if (node == NULL) {
        return "0";
    }
    switch (node->kind) {
    case NODE_LITERAL:
        return emit_literal_expression(generator, node);
    case NODE_IDENTIFIER:
        return codegen_variable_lookup(generator, node->identifier.name);
    case NODE_UNARY:
        return emit_unary_expression(generator, node);
    case NODE_BINARY:
        return emit_binary_expression(generator, node);
    case NODE_CALL:
        return emit_call_expression(generator, node);
    case NODE_MEMBER:
        return emit_member_expression(generator, node);
    case NODE_INDEX:
        return emit_index_expression(generator, node);
    case NODE_IF:
        return emit_if_expression(generator, node);
    case NODE_BLOCK:
        return emit_block_expression(generator, node);
    case NODE_STRING_INTERPOLATION:
        return emit_string_interpolation_expression(generator, node);
    case NODE_ARRAY_LITERAL:
        return emit_array_literal_expression(generator, node);
    case NODE_TUPLE_LITERAL:
        return emit_tuple_literal_expression(generator, node);
    case NODE_TYPE_CONVERSION:
        return emit_type_conversion_expression(generator, node);
    default:
        return "0";
    }
}
