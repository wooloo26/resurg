/**
 * @file lower_expr_call.c
 * @brief Call-expression lowering — direct calls, method dispatch, intrinsics, variadic args.
 */

#include "_lower.h"

// ── Intrinsic lowering ─────────────────────────────────────────────

/** Classify a callee AST node as a compiler intrinsic (or INTRINSIC_NONE). */
static IntrinsicKind classify_callee(const ASTNode *callee) {
    if (callee->kind != NODE_ID) {
        return INTRINSIC_NONE;
    }
    return intrinsic_lookup(callee->id.name);
}

/**
 * Lower print/println(arg) → HIR_CALL with INTRINSIC_PRINT/PRINTLN tag.
 *
 * Type dispatch to the backend-specific runtime fn is deferred to emit.
 */
static HirNode *lower_print_call(Lower *low, const ASTNode *ast, bool newline) {
    SrcLoc loc = ast->loc;
    int32_t arg_count = BUF_LEN(ast->call.args);
    if (arg_count == 0) {
        return hir_new(low->hir_arena, HIR_UNIT_LIT, &TYPE_UNIT_INST, loc);
    }

    HirNode *arg = lower_expr(low, ast->call.args[0]);
    if (!type_is_printable(arg->type)) {
        return hir_new(low->hir_arena, HIR_UNIT_LIT, &TYPE_UNIT_INST, loc);
    }

    IntrinsicKind kind = newline ? INTRINSIC_PRINTLN : INTRINSIC_PRINT;
    const char *fn_name = newline ? "println" : "print";
    HirNode **args = NULL;
    BUF_PUSH(args, arg);
    return lower_make_builtin_call(low,
                                   &(BuiltinCallSpec){fn_name, &TYPE_UNIT_INST, args, loc, kind});
}

/**
 * Expand assert(cond [, msg]) → rsg_assert(cond, msg, file, line).
 *
 * The msg argument defaults to NULL sentinel when absent.
 * Source location (file, line) is always injected from the call site.
 */
static HirNode *lower_assert_call(Lower *low, const ASTNode *ast) {
    int32_t arg_count = BUF_LEN(ast->call.args);
    SrcLoc loc = ast->loc;

    // Condition (default to false if missing)
    HirNode *cond;
    if (arg_count > 0) {
        cond = lower_expr(low, ast->call.args[0]);
    } else {
        cond = hir_new(low->hir_arena, HIR_BOOL_LIT, &TYPE_BOOL_INST, loc);
        cond->bool_lit.value = false;
    }

    // Message (NULL when absent; str lit kept as lit, expr passed through)
    HirNode *msg;
    if (arg_count > 1 && ast->call.args[1] != NULL) {
        msg = lower_expr(low, ast->call.args[1]);
    } else {
        // Pass NULL sentinel — codegen emits "NULL" for this
        msg = hir_new(low->hir_arena, HIR_UNIT_LIT, &TYPE_UNIT_INST, loc);
    }

    // File name as str lit
    HirNode *file_node = hir_new(low->hir_arena, HIR_STR_LIT, &TYPE_STR_INST, loc);
    file_node->str_lit.value = loc.file != NULL ? loc.file : "<unknown>";
    file_node->str_lit.len = (int32_t)strlen(file_node->str_lit.value);

    // Line number as i32 lit
    IntLitSpec line_spec = {(uint64_t)loc.line, &TYPE_I32_INST, TYPE_I32, loc};
    HirNode *line_node = lower_make_int_lit(low, &line_spec);

    HirNode **args = NULL;
    BUF_PUSH(args, cond);
    BUF_PUSH(args, msg);
    BUF_PUSH(args, file_node);
    BUF_PUSH(args, line_node);

    return lower_make_builtin_call(
        low, &(BuiltinCallSpec){RSG_FN_ASSERT, &TYPE_UNIT_INST, args, loc, INTRINSIC_ASSERT});
}

/**
 * Expand panic(msg) → rsgu_panic(msg).
 *
 * Extracts the raw C string from the RsgStr argument.
 */
static HirNode *lower_panic_call(Lower *low, const ASTNode *ast) {
    SrcLoc loc = ast->loc;
    HirNode *msg = lower_expr(low, ast->call.args[0]);

    HirNode **args = NULL;
    BUF_PUSH(args, msg);

    return lower_make_builtin_call(
        low, &(BuiltinCallSpec){RSG_FN_PANIC, &TYPE_NEVER_INST, args, loc, INTRINSIC_PANIC});
}

/**
 * Expand recover() → rsgu_recover() with option wrapping.
 *
 * The runtime function returns const char* (NULL = no panic).
 * Codegen handles the conversion to ?str.
 */
static HirNode *lower_recover_call(Lower *low, const ASTNode *ast) {
    SrcLoc loc = ast->loc;
    HirNode **args = NULL;

    return lower_make_builtin_call(
        low, &(BuiltinCallSpec){RSG_FN_RECOVER, ast->type, args, loc, INTRINSIC_RECOVER});
}

/**
 * Expand len(arg) → arg.len (struct field access) for slices/strings,
 * or a constant integer for fixed-size arrays.
 */
static HirNode *lower_len_call(Lower *low, const ASTNode *ast) {
    SrcLoc loc = ast->loc;
    if (BUF_LEN(ast->call.args) != 1) {
        return hir_new(low->hir_arena, HIR_UNIT_LIT, &TYPE_UNIT_INST, loc);
    }
    HirNode *arg = lower_expr(low, ast->call.args[0]);
    const Type *arg_type = arg->type;
    // Auto-deref: unwrap *[]T / *str
    if (arg_type != NULL && arg_type->kind == TYPE_PTR) {
        arg_type = arg_type->ptr.pointee;
    }
    // Fixed-size array: return compile-time constant
    if (arg_type != NULL && arg_type->kind == TYPE_ARRAY) {
        int32_t size = type_array_size(arg_type);
        return lower_make_int_lit(low,
                                  &(IntLitSpec){(uint64_t)size, &TYPE_I32_INST, TYPE_I32, loc});
    }
    bool via_ptr = false;
    if (arg->type != NULL && arg->type->kind == TYPE_PTR) {
        via_ptr = true;
    }
    return lower_make_field_access(
        low, &(FieldAccessSpec){arg, RSG_FIELD_LEN, &TYPE_I32_INST, via_ptr, loc});
}

// ── Intrinsic dispatch table ──────────────────────────────────────

typedef HirNode *(*IntrinsicLowerFn)(Lower *low, const ASTNode *ast);

static HirNode *lower_print_wrapper(Lower *low, const ASTNode *ast) {
    return lower_print_call(low, ast, false);
}

static HirNode *lower_println_wrapper(Lower *low, const ASTNode *ast) {
    return lower_print_call(low, ast, true);
}

static const IntrinsicLowerFn INTRINSIC_LOWER[INTRINSIC_KIND_COUNT] = {
    [INTRINSIC_PRINT] = lower_print_wrapper,  [INTRINSIC_PRINTLN] = lower_println_wrapper,
    [INTRINSIC_ASSERT] = lower_assert_call,   [INTRINSIC_PANIC] = lower_panic_call,
    [INTRINSIC_RECOVER] = lower_recover_call, [INTRINSIC_LEN] = lower_len_call,
};

// ── Method dispatch helpers ───────────────────────────────────────

/** Walk embedded structs to look up a promoted method sym. */
static HirSym *lookup_embedded_method(Lower *low, const Type *struct_type, const char *method_name,
                                      HirNode **recv_ptr, bool via_ptr) {
    for (int32_t i = 0; i < struct_type->struct_type.embed_count; i++) {
        const Type *embed_type = struct_type->struct_type.embedded[i];
        const char *embed_key =
            arena_sprintf(low->hir_arena, "%s.%s", embed_type->struct_type.name, method_name);
        HirSym *method_sym = lower_scope_lookup(low, embed_key);
        if (method_sym != NULL) {
            *recv_ptr = lower_make_field_access(
                low, &(FieldAccessSpec){*recv_ptr, embed_type->struct_type.name, embed_type,
                                        via_ptr, (*recv_ptr)->loc});
            return method_sym;
        }
    }
    return NULL;
}

/** Lower a resolved method sym into a HIR_METHOD_CALL node. */
static HirNode *lower_method_call_node(Lower *low, const ASTNode *ast, HirNode *recv,
                                       const HirSym *method_sym) {
    HirNode **args = lower_elem_list(low, ast->call.args);
    HirNode *node = hir_new(low->hir_arena, HIR_METHOD_CALL, ast->type, ast->loc);
    node->method_call.recv = method_sym->is_static ? NULL : recv;
    node->method_call.mangled_name = method_sym->mangled_name;
    node->method_call.args = args;
    node->method_call.is_ptr_recv = method_sym->is_ptr_recv;
    return node;
}

/**
 * Try to lower a member-callee call as a method call.
 *
 * Handles enum variant construction, enum method calls, and struct
 * method calls (including promoted methods from embedded structs).
 * Returns NULL when the callee is not a recognized method.
 */
static HirNode *lower_member_call(Lower *low, const ASTNode *ast) {
    const ASTNode *member_ast = ast->call.callee;
    const Type *obj_type = member_ast->member.object->type;

    bool via_ptr = false;
    if (obj_type != NULL && obj_type->kind == TYPE_PTR) {
        obj_type = obj_type->ptr.pointee;
        via_ptr = true;
    }

    // Enum: tuple variant construction, unit variant, or method call
    if (obj_type != NULL && obj_type->kind == TYPE_ENUM) {
        const char *variant_name = member_ast->member.member;
        const EnumVariant *variant = type_enum_find_variant(obj_type, variant_name);
        if (variant != NULL && variant->kind == ENUM_VARIANT_TUPLE) {
            EnumVariantSpec spec = {obj_type, variant_name, ast->loc};
            return lower_enum_tuple_init(low, &spec, ast->call.args);
        }
        if (variant != NULL && variant->kind == ENUM_VARIANT_UNIT) {
            EnumVariantSpec spec = {obj_type, variant_name, ast->loc};
            return lower_enum_unit_init(low, &spec);
        }
        const char *method_key =
            arena_sprintf(low->hir_arena, "%s.%s", type_enum_name(obj_type), variant_name);
        HirSym *method_sym = lower_scope_lookup(low, method_key);
        if (method_sym != NULL) {
            HirNode *recv = lower_expr(low, member_ast->member.object);
            return lower_method_call_node(low, ast, recv, method_sym);
        }
    }

    // Struct: direct method or promoted method from embedded struct
    if (obj_type != NULL && obj_type->kind == TYPE_STRUCT) {
        const char *method_name = member_ast->member.member;
        HirNode *recv = lower_expr(low, member_ast->member.object);

        if (!via_ptr && low->recv.sym != NULL && low->recv.is_ptr && recv->kind == HIR_VAR_REF &&
            recv->var_ref.sym == low->recv.sym) {
            via_ptr = true;
        }

        const char *key =
            arena_sprintf(low->hir_arena, "%s.%s", obj_type->struct_type.name, method_name);
        HirSym *method_sym = lower_scope_lookup(low, key);

        if (method_sym == NULL) {
            method_sym = lookup_embedded_method(low, obj_type, method_name, &recv, via_ptr);
        }

        if (method_sym != NULL) {
            return lower_method_call_node(low, ast, recv, method_sym);
        }
    }

    // Primitive ext methods: look up "type_name.method_name"
    if (obj_type != NULL) {
        const char *prim_name = type_name(low->hir_arena, obj_type);
        if (prim_name != NULL) {
            const char *method_name = member_ast->member.member;
            const char *key = arena_sprintf(low->hir_arena, "%s.%s", prim_name, method_name);
            HirSym *method_sym = lower_scope_lookup(low, key);
            if (method_sym != NULL) {
                HirNode *recv = lower_expr(low, member_ast->member.object);
                return lower_method_call_node(low, ast, recv, method_sym);
            }
        }
    }

    // Built-in .len() method on str, []T, [N]T
    if (obj_type != NULL) {
        const char *mname = member_ast->member.member;
        if (strcmp(mname, RSG_FIELD_LEN) == 0 &&
            (obj_type->kind == TYPE_SLICE || obj_type->kind == TYPE_STR ||
             obj_type->kind == TYPE_ARRAY)) {
            SrcLoc loc = ast->loc;
            HirNode *arg = lower_expr(low, member_ast->member.object);
            const Type *arg_type = arg->type;
            if (arg_type != NULL && arg_type->kind == TYPE_PTR) {
                arg_type = arg_type->ptr.pointee;
            }
            if (arg_type != NULL && arg_type->kind == TYPE_ARRAY) {
                int32_t size = type_array_size(arg_type);
                return lower_make_int_lit(
                    low, &(IntLitSpec){(uint64_t)size, &TYPE_I32_INST, TYPE_I32, loc});
            }
            bool vp = (arg->type != NULL && arg->type->kind == TYPE_PTR);
            return lower_make_field_access(
                low, &(FieldAccessSpec){arg, RSG_FIELD_LEN, &TYPE_I32_INST, vp, loc});
        }
    }

    return NULL;
}

// ── Variadic argument packing ─────────────────────────────────────

/** Build a HIR_SLICE_LIT node from variadic call-site args. */
static HirNode *lower_variadic_args(Lower *low, const ASTNode *ast, int32_t start,
                                    const Type *slice_type) {
    SrcLoc loc = ast->loc;
    int32_t arg_count = BUF_LEN(ast->call.args);
    int32_t va_count = arg_count - start;

    // Count spread vs positional args
    bool has_spread = false;
    bool has_positional = false;
    for (int32_t i = start; i < arg_count; i++) {
        bool is_spread = ast->call.arg_is_spread != NULL && ast->call.arg_is_spread[i];
        if (is_spread) {
            has_spread = true;
        } else {
            has_positional = true;
        }
    }

    // Case 1: Single spread, no positional — pass spread expression directly
    if (has_spread && !has_positional && va_count == 1) {
        return lower_expr(low, ast->call.args[start]);
    }

    // Case 2: All positional, no spread — build a HIR_SLICE_LIT
    if (!has_spread) {
        HirNode **elems = NULL;
        for (int32_t i = start; i < arg_count; i++) {
            BUF_PUSH(elems, lower_expr(low, ast->call.args[i]));
        }
        HirNode *slice_lit = hir_new(low->hir_arena, HIR_SLICE_LIT, slice_type, loc);
        slice_lit->slice_lit.elems = elems;
        return slice_lit;
    }

    // Case 3: Mixed spread + positional — build inline block that constructs slice.
    // Collects segments: each segment is either a single element or a spread slice.
    // Result is a block that: 1) computes total len, 2) allocs, 3) copies, 4) creates slice.
    // We generate:
    //   { var _seg0 = elem0; var _seg1 = spread_slice; var _seg2 = elem2; ...
    //     var _len = 1 + _seg1.len + 1;
    //     var _s = []T with capacity _len;  (emitted as a slice lit placeholder)
    //     ... copy segments into _s ... }
    // For simplicity, collect all positional into sub-groups and spreads as-is,
    // then build via concatenation of slice literals and spreads.

    // Build segments: alternating positional-group slice lits and spread expressions
    HirNode **segments = NULL;
    HirNode **pos_group = NULL;

    for (int32_t i = start; i < arg_count; i++) {
        bool is_spread = ast->call.arg_is_spread != NULL && ast->call.arg_is_spread[i];
        if (is_spread) {
            // Flush accumulated positional group as a slice lit
            if (BUF_LEN(pos_group) > 0) {
                HirNode *seg = hir_new(low->hir_arena, HIR_SLICE_LIT, slice_type, loc);
                seg->slice_lit.elems = pos_group;
                BUF_PUSH(segments, seg);
                pos_group = NULL;
            }
            BUF_PUSH(segments, lower_expr(low, ast->call.args[i]));
        } else {
            BUF_PUSH(pos_group, lower_expr(low, ast->call.args[i]));
        }
    }
    if (BUF_LEN(pos_group) > 0) {
        HirNode *seg = hir_new(low->hir_arena, HIR_SLICE_LIT, slice_type, loc);
        seg->slice_lit.elems = pos_group;
        BUF_PUSH(segments, seg);
    }

    // If zero or one segment remains, return it directly
    if (BUF_LEN(segments) <= 1) {
        return BUF_LEN(segments) == 1 ? segments[0] : NULL;
    }

    // Multiple segments: fold with binary rsg_slice_concat calls.
    // rsg_slice_concat(a, b) → CGen appends sizeof(T) automatically.
    HirNode *result = segments[0];
    for (int32_t i = 1; i < BUF_LEN(segments); i++) {
        HirNode **args = NULL;
        BUF_PUSH(args, result);
        BUF_PUSH(args, segments[i]);
        result =
            lower_make_builtin_call(low, &(BuiltinCallSpec){RSG_FN_SLICE_CONCAT, slice_type, args,
                                                            loc, INTRINSIC_SLICE_CONCAT});
    }
    return result;
}

// ── Call lowering entry point ─────────────────────────────────────

HirNode *lower_call(Lower *low, const ASTNode *ast) {
    // Dispatch compiler intrinsics via table lookup.
    IntrinsicKind intrinsic = classify_callee(ast->call.callee);
    if (intrinsic != INTRINSIC_NONE && INTRINSIC_LOWER[intrinsic] != NULL) {
        return INTRINSIC_LOWER[intrinsic](low, ast);
    }

    if (ast->call.callee->kind == NODE_MEMBER) {
        HirNode *method = lower_member_call(low, ast);
        if (method != NULL) {
            return method;
        }
    }

    // Variadic call: pack variadic args into a single slice argument
    if (ast->call.variadic_start >= 0) {
        HirNode *callee = lower_expr(low, ast->call.callee);
        HirNode **args = NULL;

        // Lower fixed args
        for (int32_t i = 0; i < ast->call.variadic_start; i++) {
            BUF_PUSH(args, lower_expr(low, ast->call.args[i]));
        }

        // Use the variadic slice type stored by the checker
        const Type *slice_type = ast->call.variadic_type;
        if (slice_type == NULL) {
            slice_type = ast->type; // fallback to return type
        }

        // Check if there are zero variadic args
        int32_t va_count = BUF_LEN(ast->call.args) - ast->call.variadic_start;
        if (va_count == 0) {
            // Empty variadic → empty slice literal
            HirNode *empty = hir_new(low->hir_arena, HIR_SLICE_LIT, slice_type, ast->loc);
            empty->slice_lit.elems = NULL;
            BUF_PUSH(args, empty);
        } else {
            BUF_PUSH(args, lower_variadic_args(low, ast, ast->call.variadic_start, slice_type));
        }

        HirNode *node = hir_new(low->hir_arena, HIR_CALL, ast->type, ast->loc);
        node->call.callee = callee;
        node->call.args = args;
        return node;
    }

    HirNode *callee = lower_expr(low, ast->call.callee);
    HirNode **args = lower_elem_list(low, ast->call.args);
    HirNode *node = hir_new(low->hir_arena, HIR_CALL, ast->type, ast->loc);
    node->call.callee = callee;
    node->call.args = args;
    return node;
}
