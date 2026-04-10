#include "core/intrinsic.h"
#include "helpers.h"
#include "repr/types.h"

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
    const char *escaped = c_str_escape(cgen, node->str_lit.value, node->str_lit.len);
    return arena_sprintf(cgen->arena, "rsg_str_new(\"%s\", %d)", escaped, node->str_lit.len);
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

/**
 * Emit print/println(arg) → rsgu_print[ln]_TYPE(arg).
 *
 * Type dispatch to the runtime fn happens here (C-backend specific).
 */
static const char *emit_print_call(CGen *cgen, const HirNode *node, bool newline) {
    const HirNode *arg = node->call.args[0];
    const char *arg_expr = emit_expr(cgen, arg);
    const char *type_suffix = type_runtime_suffix(arg->type);
    return arena_sprintf(cgen->arena, "rsgu_print%s_%s(%s)", newline ? "ln" : "", type_suffix,
                         arg_expr);
}

/**
 * Emit rsg_assert(cond, msg, file, line).
 * If the message node is HIR_UNIT_LIT (absent), emits NULL.
 */
static const char *emit_rsg_assert_call(CGen *cgen, const HirNode *node) {
    const char *cond = emit_expr(cgen, node->call.args[0]);

    const HirNode *msg_node = node->call.args[1];
    const char *msg;
    if (msg_node->kind == HIR_UNIT_LIT) {
        msg = "NULL";
    } else if (msg_node->kind == HIR_STR_LIT) {
        if (msg_node->str_lit.len == 0) {
            msg = "NULL";
        } else {
            msg = arena_sprintf(cgen->arena, "\"%s\"",
                                c_str_escape(cgen, msg_node->str_lit.value, msg_node->str_lit.len));
        }
    } else {
        const char *msg_expr = emit_expr(cgen, msg_node);
        msg = arena_sprintf(cgen->arena, "%s.data", msg_expr);
    }

    const HirNode *file_node = node->call.args[2];
    const char *file =
        arena_sprintf(cgen->arena, "\"%s\"",
                      c_str_escape(cgen, file_node->str_lit.value, file_node->str_lit.len));

    const char *line = emit_expr(cgen, node->call.args[3]);

    return arena_sprintf(cgen->arena, RSG_FN_ASSERT "(%s, %s, %s, %s)", cond, msg, file, line);
}

/**
 * Emit rsgu_panic(msg).
 * The single arg is a str expression; extract .data for the C call.
 */
static const char *emit_rsg_panic_call(CGen *cgen, const HirNode *node) {
    const HirNode *msg_node = node->call.args[0];
    const char *msg;
    if (msg_node->kind == HIR_STR_LIT) {
        msg = arena_sprintf(cgen->arena, "\"%s\"",
                            c_str_escape(cgen, msg_node->str_lit.value, msg_node->str_lit.len));
    } else {
        const char *msg_expr = emit_expr(cgen, msg_node);
        msg = arena_sprintf(cgen->arena, "%s.data", msg_expr);
    }
    return arena_sprintf(cgen->arena, RSG_FN_PANIC "(%s)", msg);
}

/**
 * Emit rsgu_recover() → ?str.
 * Calls the C rsgu_recover(), then builds an Option<str> from the result.
 */
static const char *emit_rsg_recover_call(CGen *cgen, const HirNode *node) {
    const char *opt_type = c_type_for(cgen, node->type);
    const char *tmp_raw = next_temp(cgen);
    const char *tmp_opt = next_temp(cgen);
    emit_line(cgen, "const char *%s = " RSG_FN_RECOVER "();", tmp_raw);
    emit_line(cgen, "%s %s;", opt_type, tmp_opt);
    emit_line(cgen, "if (%s != NULL) {", tmp_raw);
    cgen->indent++;
    emit_line(cgen, "%s._tag = 1;", tmp_opt);
    emit_line(cgen, "%s._data.Some._0 = rsg_str_new(%s, (int32_t)strlen(%s));", tmp_opt, tmp_raw,
              tmp_raw);
    cgen->indent--;
    emit_line(cgen, "} else {");
    cgen->indent++;
    emit_line(cgen, "%s._tag = 0;", tmp_opt);
    cgen->indent--;
    emit_line(cgen, "}");
    return tmp_opt;
}

// ── Intrinsic emit dispatch table ─────────────────────────────────

typedef const char *(*IntrinsicEmitFn)(CGen *cgen, const HirNode *node);

static const char *emit_print_wrapper(CGen *cgen, const HirNode *node) {
    return emit_print_call(cgen, node, false);
}

static const char *emit_println_wrapper(CGen *cgen, const HirNode *node) {
    return emit_print_call(cgen, node, true);
}

static const char *emit_slice_concat_call(CGen *cgen, const HirNode *node) {
    const char *a = emit_expr(cgen, node->call.args[0]);
    const char *b = emit_expr(cgen, node->call.args[1]);
    const Type *elem_type = node->type->slice.elem;
    const char *elem_c = c_type_for(cgen, elem_type);
    return arena_sprintf(cgen->arena, RSG_FN_SLICE_CONCAT "(%s, %s, sizeof(%s))", a, b, elem_c);
}

static const IntrinsicEmitFn INTRINSIC_EMIT[INTRINSIC_KIND_COUNT] = {
    [INTRINSIC_PRINT] = emit_print_wrapper,      [INTRINSIC_PRINTLN] = emit_println_wrapper,
    [INTRINSIC_ASSERT] = emit_rsg_assert_call,   [INTRINSIC_PANIC] = emit_rsg_panic_call,
    [INTRINSIC_RECOVER] = emit_rsg_recover_call, [INTRINSIC_SLICE_CONCAT] = emit_slice_concat_call,
};

static const char *emit_call_expr(CGen *cgen, const HirNode *node) {
    // Dispatch intrinsics via table lookup.
    IntrinsicKind ik = node->call.intrinsic;
    if (ik != INTRINSIC_NONE && INTRINSIC_EMIT[ik] != NULL) {
        return INTRINSIC_EMIT[ik](cgen, node);
    }

    // Indirect call through fn-typed variable: ((Ret(*)(void*,P1,...))f.fn)(f.env, args)
    const Type *callee_type = node->call.callee->type;
    bool is_indirect = false;
    if (callee_type != NULL && callee_type->kind == TYPE_FN) {
        if (node->call.callee->kind == HIR_VAR_REF &&
            node->call.callee->var_ref.sym->kind == HIR_SYM_FN) {
            is_indirect = false; // direct fn call
        } else {
            is_indirect = true;
        }
    }

    if (is_indirect) {
        const char *callee_expr = emit_expr(cgen, node->call.callee);
        const char *ret_type = c_type_for(cgen, callee_type->fn_type.return_type);
        // Build cast: (RetType(*)(void*, P1, P2, ...))
        const char *params_str = "void*";
        for (int32_t i = 0; i < callee_type->fn_type.param_count; i++) {
            const char *pt = c_type_for(cgen, callee_type->fn_type.params[i]);
            params_str = arena_sprintf(cgen->arena, "%s, %s", params_str, pt);
        }
        const char *cast = arena_sprintf(cgen->arena, "(%s(*)(%s))", ret_type, params_str);
        // Build args: f.env, arg1, arg2, ...
        const char *arg_list = arena_sprintf(cgen->arena, "%s.env", callee_expr);
        for (int32_t i = 0; i < BUF_LEN(node->call.args); i++) {
            const char *arg = emit_expr(cgen, node->call.args[i]);
            arg_list = arena_sprintf(cgen->arena, "%s, %s", arg_list, arg);
        }
        return arena_sprintf(cgen->arena, "(%s%s.fn)(%s)", cast, callee_expr, arg_list);
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
    if (type == NULL || type->kind == TYPE_UNIT || type->kind == TYPE_NEVER) {
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
    int32_t arg_count = BUF_LEN(node->method_call.args);

    // Static method call — no receiver
    if (node->method_call.recv == NULL) {
        if (arg_count > 0) {
            const char *args = join_exprs(cgen, node->method_call.args, arg_count);
            return arena_sprintf(cgen->arena, "%s(%s)", node->method_call.mangled_name, args);
        }
        return arena_sprintf(cgen->arena, "%s()", node->method_call.mangled_name);
    }

    const char *recv = emit_expr(cgen, node->method_call.recv);
    bool recv_is_ptr =
        (node->method_call.recv->type != NULL && node->method_call.recv->type->kind == TYPE_PTR) ||
        (node->method_call.recv->kind == HIR_VAR_REF &&
         node->method_call.recv->var_ref.sym != NULL &&
         node->method_call.recv->var_ref.sym->is_ptr_recv);
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
/** Shared context for a single match arm. */
typedef struct {
    const HirNode *cond;
    const HirNode *body;
    const HirNode *bindings;
    const char *result;
} MatchArm;

/** Emit bindings and body/result assignment (shared by both arm styles). */
static void emit_match_arm_body(CGen *cgen, const MatchArm *arm) {
    if (arm->bindings != NULL && arm->bindings->kind == HIR_BLOCK) {
        emit_block_stmts(cgen, arm->bindings);
    }
    if (arm->result != NULL) {
        const char *value = emit_expr(cgen, arm->body);
        emit_line(cgen, "%s = %s;", arm->result, value);
    } else {
        emit_stmt(cgen, arm->body);
    }
}

/** Wrap a condition str in parens if not already. */
static const char *wrap_cond(CGen *cgen, const char *cond_str) {
    if (cond_str[0] != '(') {
        return arena_sprintf(cgen->arena, "(%s)", cond_str);
    }
    return cond_str;
}

/** Emit a single guarded match arm (used inside do { } while(0) block). */
static void emit_match_arm_guarded(CGen *cgen, const MatchArm *arm, const HirNode *guard) {
    if (arm->cond != NULL) {
        emit_line(cgen, "if %s {", wrap_cond(cgen, emit_expr(cgen, arm->cond)));
    } else {
        emit_line(cgen, "{");
    }
    cgen->indent++;

    if (arm->bindings != NULL && arm->bindings->kind == HIR_BLOCK) {
        emit_block_stmts(cgen, arm->bindings);
    }

    if (guard != NULL) {
        emit_line(cgen, "if %s {", wrap_cond(cgen, emit_expr(cgen, guard)));
        cgen->indent++;
    }

    if (arm->result != NULL) {
        const char *value = emit_expr(cgen, arm->body);
        emit_line(cgen, "%s = %s;", arm->result, value);
    } else {
        emit_stmt(cgen, arm->body);
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
static void emit_match_arm_simple(CGen *cgen, int32_t arm_idx, const MatchArm *arm) {
    if (arm->cond != NULL) {
        const char *wrapped = wrap_cond(cgen, emit_expr(cgen, arm->cond));
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
    emit_match_arm_body(cgen, arm);
    cgen->indent--;
}

static const char *emit_match_expr(CGen *cgen, const HirNode *node) {
    const Type *type = node->type;

    emit_stmt(cgen, node->match_expr.operand);

    const char *result = NULL;
    if (type != NULL && type->kind != TYPE_UNIT && type->kind != TYPE_NEVER) {
        result = next_temp(cgen);
        emit_line(cgen, "%s %s;", c_type_for(cgen, type), result);
    }

    int32_t arm_count = BUF_LEN(node->match_expr.arms);
    bool has_any_guard = false;
    for (int32_t i = 0; i < arm_count; i++) {
        if (node->match_expr.arms[i].guard != NULL) {
            has_any_guard = true;
            break;
        }
    }

    if (has_any_guard) {
        emit_line(cgen, "do {");
        cgen->indent++;
    }

    for (int32_t i = 0; i < arm_count; i++) {
        MatchArm arm = {
            .cond = node->match_expr.arms[i].cond,
            .body = node->match_expr.arms[i].body,
            .bindings = node->match_expr.arms[i].bindings,
            .result = result,
        };
        if (has_any_guard) {
            emit_match_arm_guarded(cgen, &arm, node->match_expr.arms[i].guard);
        } else {
            emit_match_arm_simple(cgen, i, &arm);
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

// ── Fn ref and closure emission ─────────────────────────────────────

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
        // Function reference → RsgFn wrapper
        if (node->var_ref.sym->kind == HIR_SYM_FN && node->type != NULL &&
            node->type->kind == TYPE_FN) {
            return emit_fn_ref_expr(cgen, node);
        }
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
    case HIR_CLOSURE:
        return emit_closure_expr(cgen, node);
    default:
        rsg_fatal("emit_expr: unhandled HIR node kind %d", (int)node->kind);
    }
}
