#include "helpers.h"

// ── Per-node expr emitters ───────────────────────────────────────

/** Emit a comma-separated list of exprs from @p nodes into a single str. */
static const char *join_exprs(CGen *cgen, HirNode **nodes, int32_t count) {
    const char *result = "";
    for (int32_t i = 0; i < count; i++) {
        const char *elem = emit_expr(cgen, nodes[i]);
        result = (i == 0) ? elem : arena_sprintf(cgen->arena, "%s, %s", result, elem);
    }
    return result;
}

static const char *emit_bool_lit(const HirNode *node) {
    return node->bool_lit.value ? "true" : "false";
}

static const char *emit_int_lit(CGen *cgen, const HirNode *node) {
    TypeKind kind = node->int_lit.int_kind;
    uint64_t value = node->int_lit.value;
    switch (kind) {
    case TYPE_I8:
    case TYPE_I16:
    case TYPE_I32:
        return arena_sprintf(cgen->arena, "%lld", (long long)(int64_t)value);
    case TYPE_I64:
    case TYPE_I128:
    case TYPE_ISIZE:
        // Avoid C integer overflow: 9223372036854775808LL exceeds LLONG_MAX.
        if (value == (uint64_t)1 << 63) {
            return "(-9223372036854775807LL - 1)";
        }
        return arena_sprintf(cgen->arena, "%lldLL", (long long)(int64_t)value);
    case TYPE_U8:
    case TYPE_U16:
    case TYPE_U32:
        return arena_sprintf(cgen->arena, "%lluU", (unsigned long long)value);
    case TYPE_U64:
    case TYPE_U128:
    case TYPE_USIZE:
        return arena_sprintf(cgen->arena, "%lluULL", (unsigned long long)value);
    default:
        return arena_sprintf(cgen->arena, "%lld", (long long)(int64_t)value);
    }
}

static const char *emit_float_lit(CGen *cgen, const HirNode *node) {
    if (node->float_lit.float_kind == TYPE_F32) {
        return fmt_float32(cgen, node->float_lit.value);
    }
    return fmt_float64(cgen, node->float_lit.value);
}

static const char *emit_str_lit(CGen *cgen, const HirNode *node) {
    const char *escaped = c_str_escape(cgen, node->str_lit.value);
    return arena_sprintf(cgen->arena, "rsg_str_lit(\"%s\")", escaped);
}

static const char *emit_unary_expr(CGen *cgen, const HirNode *node) {
    const char *operand = emit_expr(cgen, node->unary.operand);
    if (node->unary.op == TOKEN_BANG) {
        return arena_sprintf(cgen->arena, "(!%s)", operand);
    }
    if (node->unary.op == TOKEN_MINUS) {
        return arena_sprintf(cgen->arena, "(-%s)", operand);
    }
    return operand;
}

static const char *emit_binary_expr(CGen *cgen, const HirNode *node) {
    const char *left = emit_expr(cgen, node->binary.left);
    const char *right = emit_expr(cgen, node->binary.right);
    const char *bin_op = token_kind_str(node->binary.op);
    return arena_sprintf(cgen->arena, "(%s %s %s)", left, bin_op, right);
}

/** Return true if the callee is the rsg_assert runtime fn. */
static bool is_rsg_assert_call(const HirNode *node) {
    if (node->call.callee->kind != HIR_VAR_REF) {
        return false;
    }
    return strcmp(hir_sym_name(node->call.callee->var_ref.sym), "rsg_assert") == 0;
}

/**
 * Emit rsg_assert(cond, msg, file, line).
 * Lower has already expanded assert() into rsg_assert() with four args.
 * The msg (arg 1) is either a str lit (emit as C str), a unit
 * lit (emit NULL), or a str expr (emit .data).
 * The file (arg 2) is always a str lit emitted as a C str.
 */
static const char *emit_rsg_assert_call(CGen *cgen, const HirNode *node) {
    const char *cond = emit_expr(cgen, node->call.args[0]);

    const HirNode *msg_node = node->call.args[1];
    const char *msg;
    if (msg_node->kind == HIR_UNIT_LIT) {
        msg = "NULL";
    } else if (msg_node->kind == HIR_STR_LIT) {
        msg =
            arena_sprintf(cgen->arena, "\"%s\"", c_str_escape(cgen, msg_node->str_lit.value));
    } else {
        const char *msg_expr = emit_expr(cgen, msg_node);
        msg = arena_sprintf(cgen->arena, "%s.data", msg_expr);
    }

    const HirNode *file_node = node->call.args[2];
    const char *file =
        arena_sprintf(cgen->arena, "\"%s\"", c_str_escape(cgen, file_node->str_lit.value));

    const char *line = emit_expr(cgen, node->call.args[3]);

    return arena_sprintf(cgen->arena, "rsg_assert(%s, %s, %s, %s)", cond, msg, file, line);
}

static const char *emit_call_expr(CGen *cgen, const HirNode *node) {
    if (is_rsg_assert_call(node)) {
        return emit_rsg_assert_call(cgen, node);
    }
    const char *callee;
    if (node->call.callee->kind == HIR_VAR_REF) {
        callee = node->call.callee->var_ref.sym->mangled_name;
    } else {
        callee = emit_expr(cgen, node->call.callee);
    }
    const char *arg_list = join_exprs(cgen, node->call.args, BUF_LEN(node->call.args));
    return arena_sprintf(cgen->arena, "%s(%s)", callee, arg_list);
}

/** Return true if the node is a pure expr (no side effects). */
static bool is_pure_expr(const HirNode *node) {
    if (node == NULL) {
        return false;
    }
    switch (node->kind) {
    case HIR_BOOL_LIT:
    case HIR_INT_LIT:
    case HIR_FLOAT_LIT:
    case HIR_CHAR_LIT:
    case HIR_STR_LIT:
    case HIR_UNIT_LIT:
    case HIR_VAR_REF:
        return true;
    case HIR_UNARY:
        return is_pure_expr(node->unary.operand);
    case HIR_BINARY:
        return is_pure_expr(node->binary.left) && is_pure_expr(node->binary.right);
    default:
        return false;
    }
}

/** Return the simple result expr from a branch body, or NULL. */
static const HirNode *simple_branch_result(const HirNode *body) {
    if (body == NULL) {
        return NULL;
    }
    if (body->kind == HIR_BLOCK) {
        if (BUF_LEN(body->block.stmts) != 0 || body->block.result == NULL) {
            return NULL;
        }
        return body->block.result;
    }
    return body;
}

/** Return true if a HIR_IF can be emitted as a C ternary. */
static bool is_ternary_candidate(const HirNode *node) {
    if (node->kind != HIR_IF || node->if_expr.else_body == NULL) {
        return false;
    }
    const HirNode *then_result = simple_branch_result(node->if_expr.then_body);
    if (then_result == NULL || !is_pure_expr(then_result)) {
        return false;
    }
    const HirNode *else_body = node->if_expr.else_body;
    if (else_body->kind == HIR_IF) {
        return false;
    }
    const HirNode *else_result = simple_branch_result(else_body);
    return else_result != NULL && is_pure_expr(else_result);
}

static const char *emit_if_expr(CGen *cgen, const HirNode *node) {
    // Emit as C ternary when both branches are pure single-expr results
    if (is_ternary_candidate(node)) {
        const char *cond = emit_expr(cgen, node->if_expr.cond);
        const HirNode *then_result = simple_branch_result(node->if_expr.then_body);
        const HirNode *else_result = simple_branch_result(node->if_expr.else_body);
        const char *then_value = emit_expr(cgen, then_result);
        const char *else_value = emit_expr(cgen, else_result);
        return arena_sprintf(cgen->arena, "(%s ? %s : %s)", cond, then_value, else_value);
    }

    const Type *type = node->type;
    if (type == NULL || type->kind == TYPE_UNIT) {
        emit_if(cgen, node, NULL, false);
        return "(void)0";
    }
    const char *temp = next_temp(cgen);
    emit_line(cgen, "%s %s;", c_type_for(cgen, type), temp);
    emit_if(cgen, node, temp, false);
    return temp;
}

static const char *emit_block_expr(CGen *cgen, const HirNode *node) {
    emit_block_stmts(cgen, node);
    if (node->block.result != NULL) {
        return emit_expr(cgen, node->block.result);
    }
    return "(void)0";
}

static const char *emit_array_lit_expr(CGen *cgen, const HirNode *node) {
    const char *tname = c_type_for(cgen, node->type);
    const char *elems = join_exprs(cgen, node->array_lit.elems, BUF_LEN(node->array_lit.elems));
    return arena_sprintf(cgen->arena, "(%s){ ._data = { %s } }", tname, elems);
}

static const char *emit_slice_lit_expr(CGen *cgen, const HirNode *node) {
    int32_t count = BUF_LEN(node->slice_lit.elems);
    const Type *elem_type = node->type->slice.elem;
    const char *elem_c = c_type_for(cgen, elem_type);
    if (count == 0) {
        return arena_sprintf(cgen->arena, "rsg_slice_new(NULL, 0, sizeof(%s))", elem_c);
    }
    const char *elems = join_exprs(cgen, node->slice_lit.elems, count);
    return arena_sprintf(cgen->arena, "rsg_slice_new((%s[]){ %s }, %d, sizeof(%s))", elem_c, elems,
                         count, elem_c);
}

static const char *emit_slice_expr_expr(CGen *cgen, const HirNode *node) {
    const char *object = emit_expr(cgen, node->slice_expr.object);
    const Type *obj_type = node->slice_expr.object->type;
    const Type *elem_type = node->type->slice.elem;
    const char *elem_c = c_type_for(cgen, elem_type);

    if (node->slice_expr.from_array) {
        int32_t array_size = obj_type->array.size;
        return arena_sprintf(cgen->arena, "rsg_slice_from_array(%s._data, %d, sizeof(%s))", object,
                             array_size, elem_c);
    }

    const char *start_str = "0";
    if (node->slice_expr.start != NULL) {
        start_str = emit_expr(cgen, node->slice_expr.start);
    }
    const char *end_str;
    if (node->slice_expr.end != NULL) {
        end_str = emit_expr(cgen, node->slice_expr.end);
    } else {
        end_str = arena_sprintf(cgen->arena, "%s.len", object);
    }
    return arena_sprintf(cgen->arena, "rsg_slice_sub(%s, %s, %s, sizeof(%s))", object, start_str,
                         end_str, elem_c);
}

static const char *emit_tuple_lit_expr(CGen *cgen, const HirNode *node) {
    const char *tname = c_type_for(cgen, node->type);
    const char *result = arena_sprintf(cgen->arena, "(%s){ ", tname);
    for (int32_t i = 0; i < BUF_LEN(node->tuple_lit.elems); i++) {
        if (i > 0) {
            result = arena_sprintf(cgen->arena, "%s, ", result);
        }
        const char *elem = emit_expr(cgen, node->tuple_lit.elems[i]);
        result = arena_sprintf(cgen->arena, "%s._%d = %s", result, i, elem);
    }
    return arena_sprintf(cgen->arena, "%s }", result);
}

static const char *emit_idx_expr(CGen *cgen, const HirNode *node) {
    const char *object = emit_expr(cgen, node->idx_access.object);
    const char *idx = emit_expr(cgen, node->idx_access.idx);
    const Type *obj_type = node->idx_access.object->type;
    if (obj_type != NULL && obj_type->kind == TYPE_SLICE) {
        const char *elem_c_type = c_type_for(cgen, node->type);
        return arena_sprintf(cgen->arena, "((%s*)%s.data)[%s]", elem_c_type, object, idx);
    }
    return arena_sprintf(cgen->arena, "%s._data[%s]", object, idx);
}

static const char *emit_tuple_idx_expr(CGen *cgen, const HirNode *node) {
    const char *object = emit_expr(cgen, node->tuple_idx.object);
    return arena_sprintf(cgen->arena, "%s._%d", object, node->tuple_idx.elem_idx);
}

static const char *emit_type_conversion_expr(CGen *cgen, const HirNode *node) {
    return emit_expr(cgen, node->type_conversion.operand);
}

static const char *emit_module_access_expr(CGen *cgen, const HirNode *node) {
    const char *object = emit_expr(cgen, node->module_access.object);
    return arena_sprintf(cgen->arena, "%s._%s", object, node->module_access.member);
}

static const char *emit_struct_lit_expr(CGen *cgen, const HirNode *node) {
    const char *tname = c_type_for(cgen, node->type);
    const char *result = arena_sprintf(cgen->arena, "(%s){ ", tname);
    for (int32_t i = 0; i < BUF_LEN(node->struct_lit.field_names); i++) {
        if (i > 0) {
            result = arena_sprintf(cgen->arena, "%s, ", result);
        }
        const char *value = emit_expr(cgen, node->struct_lit.field_values[i]);
        result = arena_sprintf(cgen->arena, "%s.%s = %s", result, node->struct_lit.field_names[i],
                               value);
    }
    return arena_sprintf(cgen->arena, "%s }", result);
}

static const char *emit_struct_field_access_expr(CGen *cgen, const HirNode *node) {
    const char *object = emit_expr(cgen, node->struct_field_access.object);
    if (node->struct_field_access.via_ptr) {
        return arena_sprintf(cgen->arena, "%s->%s", object, node->struct_field_access.field);
    }
    return arena_sprintf(cgen->arena, "%s.%s", object, node->struct_field_access.field);
}

static const char *emit_method_call_expr(CGen *cgen, const HirNode *node) {
    const char *recv = emit_expr(cgen, node->method_call.recv);
    int32_t arg_count = BUF_LEN(node->method_call.args);
    bool recv_is_ptr =
        node->method_call.recv->type != NULL && node->method_call.recv->type->kind == TYPE_PTR;
    const char *recv_expr;
    if (node->method_call.is_ptr_recv) {
        // Pointer recv: needs a ptr
        recv_expr = recv_is_ptr ? recv : arena_sprintf(cgen->arena, "&(%s)", recv);
    } else {
        // Value recv: needs a value (copy)
        recv_expr = recv_is_ptr ? arena_sprintf(cgen->arena, "(*%s)", recv) : recv;
    }
    if (arg_count > 0) {
        const char *args = join_exprs(cgen, node->method_call.args, arg_count);
        return arena_sprintf(cgen->arena, "%s(%s, %s)", node->method_call.mangled_name, recv_expr,
                             args);
    }
    return arena_sprintf(cgen->arena, "%s(%s)", node->method_call.mangled_name, recv_expr);
}

static const char *emit_heap_alloc_expr(CGen *cgen, const HirNode *node) {
    const char *pointee_type = c_type_for(cgen, node->type->ptr.pointee);
    const char *tmp = next_temp(cgen);
    const char *inner = emit_expr(cgen, node->heap_alloc.operand);
    emit_line(cgen, "%s *%s = rsg_heap_alloc(sizeof(%s));", pointee_type, tmp, pointee_type);
    emit_line(cgen, "*%s = %s;", tmp, inner);
    return tmp;
}

static const char *emit_address_of_expr(CGen *cgen, const HirNode *node) {
    const char *operand = emit_expr(cgen, node->address_of.operand);
    return arena_sprintf(cgen->arena, "&(%s)", operand);
}

static const char *emit_deref_expr(CGen *cgen, const HirNode *node) {
    const char *operand = emit_expr(cgen, node->deref.operand);
    return arena_sprintf(cgen->arena, "(*%s)", operand);
}

/** Emit a match expr as a temp var with an if-else chain. */
/** Emit a single guarded match arm (used inside do { } while(0) block). */
static void emit_match_arm_guarded(CGen *cgen, const HirNode *cond, const HirNode *guard,
                                   const HirNode *body, const HirNode *bindings, const char *result) {
    if (cond != NULL) {
        const char *cond_str = emit_expr(cgen, cond);
        const char *wrapped = cond_str;
        if (cond_str[0] != '(') {
            wrapped = arena_sprintf(cgen->arena, "(%s)", cond_str);
        }
        emit_line(cgen, "if %s {", wrapped);
    } else {
        emit_line(cgen, "{");
    }
    cgen->indent++;

    if (bindings != NULL && bindings->kind == HIR_BLOCK) {
        emit_block_stmts(cgen, bindings);
    }

    if (guard != NULL) {
        const char *guard_str = emit_expr(cgen, guard);
        const char *gwrapped = guard_str;
        if (guard_str[0] != '(') {
            gwrapped = arena_sprintf(cgen->arena, "(%s)", guard_str);
        }
        emit_line(cgen, "if %s {", gwrapped);
        cgen->indent++;
    }

    if (result != NULL) {
        const char *value = emit_expr(cgen, body);
        emit_line(cgen, "%s = %s;", result, value);
    } else {
        emit_stmt(cgen, body);
    }
    emit_line(cgen, "break;");

    if (guard != NULL) {
        cgen->indent--;
        emit_line(cgen, "}");
    }
    cgen->indent--;
    emit_line(cgen, "}");
}

/** Emit a single simple match arm (part of if-else chain, no guards). */
static void emit_match_arm_simple(CGen *cgen, int32_t arm_idx, const HirNode *cond,
                                  const HirNode *body, const HirNode *bindings, const char *result) {
    if (cond != NULL) {
        const char *cond_str = emit_expr(cgen, cond);
        const char *wrapped = cond_str;
        if (cond_str[0] != '(') {
            wrapped = arena_sprintf(cgen->arena, "(%s)", cond_str);
        }
        if (arm_idx == 0) {
            emit_line(cgen, "if %s {", wrapped);
        } else {
            emit_indent(cgen);
            fprintf(cgen->output, "} else if %s {\n", wrapped);
        }
    } else {
        if (arm_idx == 0) {
            emit_line(cgen, "{");
        } else {
            emit_line(cgen, "} else {");
        }
    }
    cgen->indent++;

    if (bindings != NULL && bindings->kind == HIR_BLOCK) {
        emit_block_stmts(cgen, bindings);
    }

    if (result != NULL) {
        const char *value = emit_expr(cgen, body);
        emit_line(cgen, "%s = %s;", result, value);
    } else {
        emit_stmt(cgen, body);
    }
    cgen->indent--;
}

static const char *emit_match_expr(CGen *cgen, const HirNode *node) {
    const Type *type = node->type;

    emit_stmt(cgen, node->match_expr.operand);

    const char *result = NULL;
    if (type != NULL && type->kind != TYPE_UNIT) {
        result = next_temp(cgen);
        emit_line(cgen, "%s %s;", c_type_for(cgen, type), result);
    }

    int32_t arm_count = BUF_LEN(node->match_expr.arm_conds);
    bool has_any_guard = false;
    for (int32_t i = 0; i < arm_count; i++) {
        if (node->match_expr.arm_guards[i] != NULL) {
            has_any_guard = true;
            break;
        }
    }

    if (has_any_guard) {
        emit_line(cgen, "do {");
        cgen->indent++;
    }

    for (int32_t i = 0; i < arm_count; i++) {
        if (has_any_guard) {
            emit_match_arm_guarded(cgen, node->match_expr.arm_conds[i],
                                   node->match_expr.arm_guards[i], node->match_expr.arm_bodies[i],
                                   node->match_expr.arm_bindings[i], result);
        } else {
            emit_match_arm_simple(cgen, i, node->match_expr.arm_conds[i],
                                  node->match_expr.arm_bodies[i], node->match_expr.arm_bindings[i],
                                  result);
        }
    }

    if (has_any_guard) {
        cgen->indent--;
        emit_line(cgen, "} while (0);");
    } else {
        emit_line(cgen, "}");
    }

    return result != NULL ? result : "(void)0";
}

// ── Expression dispatch ────────────────────────────────────────────────

const char *emit_expr(CGen *cgen, const HirNode *node) {
    if (node == NULL) {
        return "0";
    }
    switch (node->kind) {
    case HIR_BOOL_LIT:
        return emit_bool_lit(node);
    case HIR_INT_LIT:
        return emit_int_lit(cgen, node);
    case HIR_FLOAT_LIT:
        return emit_float_lit(cgen, node);
    case HIR_CHAR_LIT:
        return c_char_escape(cgen, node->char_lit.value);
    case HIR_STR_LIT:
        return emit_str_lit(cgen, node);
    case HIR_UNIT_LIT:
        return "(void)0";
    case HIR_VAR_REF:
        return node->var_ref.sym->mangled_name;
    case HIR_UNARY:
        return emit_unary_expr(cgen, node);
    case HIR_BINARY:
        return emit_binary_expr(cgen, node);
    case HIR_CALL:
        return emit_call_expr(cgen, node);
    case HIR_MODULE_ACCESS:
        return emit_module_access_expr(cgen, node);
    case HIR_IDX:
        return emit_idx_expr(cgen, node);
    case HIR_TUPLE_IDX:
        return emit_tuple_idx_expr(cgen, node);
    case HIR_IF:
        return emit_if_expr(cgen, node);
    case HIR_BLOCK:
        return emit_block_expr(cgen, node);
    case HIR_ARRAY_LIT:
        return emit_array_lit_expr(cgen, node);
    case HIR_SLICE_LIT:
        return emit_slice_lit_expr(cgen, node);
    case HIR_SLICE_EXPR:
        return emit_slice_expr_expr(cgen, node);
    case HIR_TUPLE_LIT:
        return emit_tuple_lit_expr(cgen, node);
    case HIR_TYPE_CONVERSION:
        return emit_type_conversion_expr(cgen, node);
    case HIR_STRUCT_LIT:
        return emit_struct_lit_expr(cgen, node);
    case HIR_STRUCT_FIELD_ACCESS:
        return emit_struct_field_access_expr(cgen, node);
    case HIR_METHOD_CALL:
        return emit_method_call_expr(cgen, node);
    case HIR_HEAP_ALLOC:
        return emit_heap_alloc_expr(cgen, node);
    case HIR_ADDRESS_OF:
        return emit_address_of_expr(cgen, node);
    case HIR_DEREF:
        return emit_deref_expr(cgen, node);
    case HIR_MATCH:
        return emit_match_expr(cgen, node);
    default:
        return "0";
    }
}
