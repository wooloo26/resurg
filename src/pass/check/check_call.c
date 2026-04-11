#include "_check.h"

/**
 * @file check_call.c
 * @brief Call-expression type checking — direct calls, named args, tuple structs, variants.
 */

// ── Named-arg helpers ─────────────────────────────────────────────

/** Find the index of a named parameter in @p sig. Returns -1 if not found. */
static int32_t find_param_index(const FnSig *sig, const char *name) {
    for (int32_t j = 0; j < sig->param_count; j++) {
        if (strcmp(sig->param_names[j], name) == 0) {
            return j;
        }
    }
    return -1;
}

/**
 * Reorder call args to match param positions using named labels.
 * Clears @p node->call.arg_names after reordering.
 */
static void reorder_named_args(Sema *sema, ASTNode *node, const FnSig *sig) {
    int32_t arg_count = BUF_LEN(node->call.args);
    if (node->call.arg_names == NULL || arg_count == 0) {
        return;
    }

    // Skip reordering when no arg is actually named (all positional).
    bool has_named = false;
    for (int32_t i = 0; i < arg_count; i++) {
        if (node->call.arg_names[i] != NULL) {
            has_named = true;
            break;
        }
    }
    if (!has_named) {
        node->call.arg_names = NULL;
        return;
    }

    ASTNode **reordered = NULL;
    for (int32_t i = 0; i < sig->param_count; i++) {
        BUF_PUSH(reordered, (ASTNode *)NULL);
    }
    if (reordered == NULL) {
        return;
    }
    for (int32_t i = 0; i < arg_count; i++) {
        const char *aname = node->call.arg_names[i];
        if (aname != NULL) {
            int32_t idx = find_param_index(sig, aname);
            if (idx < 0) {
                SEMA_ERR(sema, node->call.args[i]->loc, "no parameter named '%s'", aname);
            } else {
                reordered[idx] = node->call.args[i];
            }
        } else if (i < BUF_LEN(reordered)) {
            reordered[i] = node->call.args[i];
        } else {
            // Excess positional args beyond param_count (variadic) — append.
            BUF_PUSH(reordered, node->call.args[i]);
        }
    }
    node->call.args = reordered;
    node->call.arg_names = NULL;
}

/**
 * Validate arg count against @p sig, promote lit args, and type-check each arg.
 * Handles variadic parameters: when the last param is variadic (..T → []T),
 * remaining call-site args are checked against the element type.
 */
static void check_and_promote_call_args(Sema *sema, ASTNode *node, const FnSig *sig) {
    int32_t arg_count = BUF_LEN(node->call.args);
    int32_t fixed_count = sig->has_variadic ? sig->param_count - 1 : sig->param_count;

    if (sig->has_variadic) {
        if (arg_count < fixed_count) {
            SEMA_ERR(sema, node->loc, "expected at least %d args, got %d", fixed_count, arg_count);
            return;
        }
    } else {
        if (arg_count < sig->param_count) {
            SEMA_ERR(sema, node->loc, "expected %d args, got %d", sig->param_count, arg_count);
            return;
        }
        if (arg_count > sig->param_count) {
            SEMA_ERR(sema, node->loc, "expected %d args, got %d", sig->param_count, arg_count);
            return;
        }
    }

    // Check fixed (non-variadic) args
    for (int32_t i = 0; i < fixed_count && i < arg_count; i++) {
        ASTNode *arg = node->call.args[i];
        if (arg == NULL) {
            continue;
        }
        const Type *param_type = sig->param_types[i];
        promote_lit(arg, param_type);
        const Type *arg_type = arg->type;
        if (arg_type != NULL && param_type != NULL && !type_assignable(arg_type, param_type) &&
            arg_type->kind != TYPE_ERR && param_type->kind != TYPE_ERR) {
            SEMA_ERR(sema, arg->loc, "type mismatch: expected '%s', got '%s'",
                     type_name(sema->base.arena, param_type),
                     type_name(sema->base.arena, arg_type));
        }
    }

    // Check variadic args against the slice element type
    if (sig->has_variadic && arg_count > fixed_count) {
        const Type *slice_type = sig->param_types[sig->param_count - 1];
        const Type *elem_type =
            (slice_type != NULL && slice_type->kind == TYPE_SLICE) ? slice_type->slice.elem : NULL;
        for (int32_t i = fixed_count; i < arg_count; i++) {
            ASTNode *arg = node->call.args[i];
            if (arg == NULL || elem_type == NULL) {
                continue;
            }
            bool is_spread = node->call.arg_is_spread != NULL && node->call.arg_is_spread[i];
            if (is_spread) {
                // Spread arg must be a []T matching the variadic slice type
                promote_lit(arg, slice_type);
                const Type *arg_type = arg->type;
                if (arg_type != NULL && !type_assignable(arg_type, slice_type) &&
                    arg_type->kind != TYPE_ERR) {
                    SEMA_ERR(sema, arg->loc, "type mismatch: expected '%s', got '%s'",
                             type_name(sema->base.arena, slice_type),
                             type_name(sema->base.arena, arg_type));
                }
            } else {
                // Individual arg must match element type
                promote_lit(arg, elem_type);
                const Type *arg_type = arg->type;
                if (arg_type != NULL && !type_assignable(arg_type, elem_type) &&
                    arg_type->kind != TYPE_ERR) {
                    SEMA_ERR(sema, arg->loc, "type mismatch: expected '%s', got '%s'",
                             type_name(sema->base.arena, elem_type),
                             type_name(sema->base.arena, arg_type));
                }
            }
        }
    }

    // Annotate the call for the lower pass
    if (sig->has_variadic) {
        node->call.variadic_start = fixed_count;
        node->call.variadic_type = sig->param_types[sig->param_count - 1];
    }
}

/** Validate a call through a fn-typed value (variable, field, or expression). */
const Type *check_fn_type_call(Sema *sema, ASTNode *node, const Type *fn_type) {
    int32_t arg_count = BUF_LEN(node->call.args);
    int32_t fn_param_count = fn_type->fn_type.param_count;
    if (arg_count != fn_param_count) {
        SEMA_ERR(sema, node->loc, "expected %d args, got %d", fn_param_count, arg_count);
    } else {
        for (int32_t i = 0; i < arg_count; i++) {
            const Type *param_type = fn_type->fn_type.params[i];
            promote_lit(node->call.args[i], param_type);
            const Type *arg_type = node->call.args[i]->type;
            if (arg_type != NULL && param_type != NULL && !type_assignable(arg_type, param_type) &&
                arg_type->kind != TYPE_ERR && param_type->kind != TYPE_ERR) {
                SEMA_ERR(sema, node->call.args[i]->loc, "type mismatch: expected '%s', got '%s'",
                         type_name(sema->base.arena, param_type),
                         type_name(sema->base.arena, arg_type));
            }
        }
    }
    return fn_type->fn_type.return_type;
}

// ── Enum variant call ─────────────────────────────────────────────

/** Check an enum tuple variant construction: Enum.Variant(args). */
const Type *check_enum_variant_call(Sema *sema, ASTNode *node, const Type *enum_type,
                                    const char *variant_name) {
    const EnumVariant *variant = type_enum_find_variant(enum_type, variant_name);
    if (variant == NULL) {
        return NULL;
    }
    if (variant->kind == ENUM_VARIANT_UNIT) {
        // Unit variant: no args expected
        int32_t arg_count = BUF_LEN(node->call.args);
        if (arg_count != 0) {
            SEMA_ERR(sema, node->loc, "unit variant '%s' takes no arguments", variant_name);
        }
        node->type = enum_type;
        return enum_type;
    }
    if (variant->kind != ENUM_VARIANT_TUPLE) {
        return NULL;
    }
    int32_t arg_count = BUF_LEN(node->call.args);
    if (arg_count != variant->tuple_count) {
        SEMA_ERR(sema, node->loc, "expected %d args for variant '%s', got %d", variant->tuple_count,
                 variant_name, arg_count);
    } else {
        for (int32_t i = 0; i < arg_count; i++) {
            promote_lit(node->call.args[i], variant->tuple_types[i]);
            const Type *arg_type = node->call.args[i]->type;
            if (arg_type != NULL && !type_equal(arg_type, variant->tuple_types[i]) &&
                arg_type->kind != TYPE_ERR) {
                SEMA_ERR(sema, node->call.args[i]->loc, "type mismatch: expected '%s', got '%s'",
                         type_name(sema->base.arena, variant->tuple_types[i]),
                         type_name(sema->base.arena, arg_type));
            }
        }
    }
    node->type = enum_type;
    return enum_type;
}

// ── Resolved call application ─────────────────────────────────────

/** Reorder named args, validate, and apply a resolved fn sig to a call. */
const Type *resolve_call(Sema *sema, ASTNode *node, const FnSig *sig) {
    reorder_named_args(sema, node, sig);
    check_and_promote_call_args(sema, node, sig);
    node->type = sig->return_type;
    return sig->return_type;
}

// ── Tuple struct constructors ─────────────────────────────────────

/**
 * Handle `Name(value)` or `Name<T>(value)` as a tuple struct constructor.
 * Rewrites the call node to NODE_STRUCT_LIT on success.
 */
/** Infer generic type args for a tuple struct from the argument types. */
static StructDef *infer_tuple_struct_type_args(Sema *sema, ASTNode *node, const char **fn_name) {
    GenericStructDef *gdef = sema_lookup_generic_struct(sema, *fn_name);
    if (gdef == NULL || BUF_LEN(node->call.args) != gdef->type_param_count) {
        return NULL;
    }
    // Build type args from argument types
    ASTType *inferred_args = NULL;
    for (int32_t i = 0; i < BUF_LEN(node->call.args); i++) {
        const Type *arg_type = node->call.args[i]->type;
        if (arg_type == NULL || arg_type->kind == TYPE_ERR) {
            BUF_FREE(inferred_args);
            return NULL;
        }
        ASTType ta = {.kind = AST_TYPE_NAME,
                      .name = type_name(sema->base.arena, arg_type),
                      .type_args = NULL,
                      .loc = node->loc};
        hash_table_insert(&sema->base.generics.type_params, ta.name, (void *)arg_type);
        BUF_PUSH(inferred_args, ta);
    }
    GenericInstArgs inst_args = {inferred_args, BUF_LEN(inferred_args), node->loc};
    const char *mangled = instantiate_generic_struct(sema, gdef, &inst_args);
    for (int32_t i = 0; i < BUF_LEN(inferred_args); i++) {
        hash_table_remove(&sema->base.generics.type_params, inferred_args[i].name);
    }
    BUF_FREE(inferred_args);
    if (mangled != NULL) {
        *fn_name = mangled;
        return sema_lookup_struct(sema, mangled);
    }
    return NULL;
}

static const Type *check_tuple_struct_call(Sema *sema, ASTNode *node, const char *fn_name) {
    // Resolve Self to enclosing type name
    if (strcmp(fn_name, "Self") == 0 && sema->infer.self_type_name != NULL) {
        fn_name = sema->infer.self_type_name;
        node->call.callee->id.name = fn_name;
    }

    StructDef *sdef = sema_lookup_struct(sema, fn_name);

    // Try generic instantiation with explicit type args
    if (sdef == NULL && BUF_LEN(node->call.type_args) > 0) {
        GenericStructDef *gdef = sema_lookup_generic_struct(sema, fn_name);
        if (gdef != NULL) {
            GenericInstArgs inst_args = {node->call.type_args, BUF_LEN(node->call.type_args),
                                         node->loc};
            const char *mangled = instantiate_generic_struct(sema, gdef, &inst_args);
            if (mangled != NULL) {
                sdef = sema_lookup_struct(sema, mangled);
                fn_name = mangled;
            }
        }
    }

    // Try generic inference from arg types
    if (sdef == NULL && BUF_LEN(node->call.type_args) == 0) {
        sdef = infer_tuple_struct_type_args(sema, node, &fn_name);
    }

    if (sdef == NULL || !sdef->is_tuple_struct) {
        return NULL;
    }

    int32_t field_count = sdef->type->struct_type.field_count;
    int32_t arg_count = BUF_LEN(node->call.args);
    if (arg_count != field_count) {
        SEMA_ERR(sema, node->loc, "tuple struct '%s' expects %d argument(s), got %d", fn_name,
                 field_count, arg_count);
        return &TYPE_ERR_INST;
    }

    // Type-check each argument against the corresponding _N field
    for (int32_t i = 0; i < field_count; i++) {
        const Type *expected = sdef->type->struct_type.fields[i].type;
        const Type *actual = node->call.args[i]->type;
        // Re-check arg with expected type if initial check failed (e.g., bare None)
        if (actual == NULL || actual->kind == TYPE_ERR) {
            SEMA_INFER_SCOPE(sema, expected_type, expected);
            actual = check_node(sema, node->call.args[i]);
            SEMA_INFER_RESTORE(sema, expected_type);
        }
        if (actual == NULL || actual->kind == TYPE_ERR) {
            continue;
        }
        if (!type_assignable(expected, actual)) {
            promote_lit(node->call.args[i], expected);
            actual = node->call.args[i]->type;
            if (!type_assignable(expected, actual)) {
                SEMA_ERR(sema, node->call.args[i]->loc,
                         "type mismatch in tuple struct '%s' field %d: expected '%s', got '%s'",
                         fn_name, i, type_name(sema->base.arena, expected),
                         type_name(sema->base.arena, actual));
                return &TYPE_ERR_INST;
            }
        }
    }

    // Rewrite NODE_CALL → NODE_STRUCT_LIT
    node->kind = NODE_STRUCT_LIT;
    const char **field_names = NULL;
    ASTNode **field_values = NULL;
    for (int32_t i = 0; i < field_count; i++) {
        BUF_PUSH(field_names, sdef->type->struct_type.fields[i].name);
        BUF_PUSH(field_values, node->call.args[i]);
    }
    node->struct_lit.name = fn_name;
    node->struct_lit.field_names = field_names;
    node->struct_lit.field_values = field_values;
    node->struct_lit.type_args = NULL;
    node->type = sdef->type;
    return sdef->type;
}

// ── Bare variant constructors ─────────────────────────────────────

/** Rewrite callee to a member access and check as enum variant call. */
static const Type *rewrite_variant_call(Sema *sema, ASTNode *node, const Type *enum_type,
                                        const char *variant_name) {
    ASTNode *callee_member = ast_new(sema->base.arena, NODE_MEMBER, node->loc);
    callee_member->member.object = ast_new(sema->base.arena, NODE_ID, node->loc);
    callee_member->member.object->id.name = type_enum_name(enum_type);
    callee_member->member.object->type = enum_type;
    callee_member->member.member = variant_name;
    node->call.callee = callee_member;
    return check_enum_variant_call(sema, node, enum_type, variant_name);
}

/**
 * Handle bare variant calls (e.g. Some(x), Ok(x), Err(x)) via the
 * variant constructor table populated by `pub use Enum::*`.
 *
 * Resolution order:
 *  1. Context-based — expected_type or fn_return_type provides the concrete enum.
 *  2. Inference — for single-type-param generic enums, infer from arg type.
 */
static const Type *check_bare_variant_call(Sema *sema, ASTNode *node, const char *fn_name) {
    if (BUF_LEN(node->call.args) != 1) {
        return NULL;
    }

    VariantCtorInfo *vci = hash_table_lookup(&sema->base.db.variant_ctor_table, fn_name);
    if (vci == NULL || !vci->has_payload) {
        return NULL;
    }

    // Inference-based: for single-type-param generic enums, infer T from the arg.
    // Tried first so nested calls like Some(Some(42)) resolve inside-out.
    GenericEnumDef *gdef = sema_lookup_generic_enum(sema, vci->enum_name);
    if (gdef != NULL && gdef->type_param_count == 1) {
        const Type *arg_type = node->call.args[0]->type;
        if (arg_type != NULL && arg_type->kind != TYPE_ERR) {
            ASTType val_arg = {.kind = AST_TYPE_NAME,
                               .name = type_name(sema->base.arena, arg_type),
                               .type_args = NULL,
                               .loc = node->loc};
            hash_table_insert(&sema->base.generics.type_params, val_arg.name, (void *)arg_type);
            GenericInstArgs inst_args = {&val_arg, 1, node->loc};
            const char *mangled = instantiate_generic_enum(sema, gdef, &inst_args);
            hash_table_remove(&sema->base.generics.type_params, val_arg.name);

            if (mangled != NULL) {
                const Type *enum_type = sema_lookup_type_alias(sema, mangled);
                if (enum_type != NULL) {
                    return rewrite_variant_call(sema, node, enum_type, fn_name);
                }
            }
        }
    }

    // Context-based fallback: expected_type or fn_return_type provides the concrete enum.
    // Used for multi-param generics like Result<T,E> where full inference is not possible.
    const Type *ctx = sema->infer.expected_type;
    if (ctx == NULL) {
        ctx = sema->infer.fn_return_type;
    }
    if (ctx != NULL && ctx->kind == TYPE_ENUM) {
        const EnumVariant *variant = type_enum_find_variant(ctx, fn_name);
        if (variant != NULL) {
            return rewrite_variant_call(sema, node, ctx, fn_name);
        }
    }

    return NULL;
}

// ── Arg checking with expected-type propagation ───────────────────

/** Check args, propagating expected param types so closures can infer. */
static void check_call_args(Sema *sema, ASTNode *node, const char *fn_name) {
    FnSig *early_sig = (fn_name != NULL) ? sema_lookup_fn(sema, fn_name) : NULL;
    const Type *callee_fn_type = NULL;
    if (early_sig == NULL && fn_name != NULL) {
        Sym *callee_sym = scope_lookup(sema, fn_name);
        if (callee_sym != NULL && callee_sym->type != NULL && callee_sym->type->kind == TYPE_FN) {
            callee_fn_type = callee_sym->type;
        }
    }
    for (int32_t i = 0; i < BUF_LEN(node->call.args); i++) {
        const Type *arg_expected = sema->infer.expected_type;
        if (early_sig != NULL && i < early_sig->param_count) {
            arg_expected = early_sig->param_types[i];
        } else if (callee_fn_type != NULL && i < callee_fn_type->fn_type.param_count) {
            arg_expected = callee_fn_type->fn_type.params[i];
        }
        SEMA_INFER_SCOPE(sema, expected_type, arg_expected);
        check_node(sema, node->call.args[i]);
        SEMA_INFER_RESTORE(sema, expected_type);
    }
}

// ── Two-phase call dispatch ───────────────────────────────────────

/** Discriminator for call classification. */
typedef enum {
    CALL_CLOSURE,      // |x| expr(args) — inline closure
    CALL_METHOD,       // obj.method(args) — member dispatch
    CALL_GENERIC,      // fn<T>(args) or Name<T>(args)
    CALL_FN,           // named function call
    CALL_FN_TYPE,      // call through fn-typed variable/field
    CALL_TUPLE_CTOR,   // TupleStruct(arg, ...)
    CALL_BARE_VARIANT, // Some(x), Ok(x), Err(x)
    CALL_EXPR,         // (expr)(args) — general expression callee
    CALL_UNKNOWN,      // unresolved — will emit an error
} CallKind;

/** Phase 1: classify the call to determine the dispatch path. */
static CallKind classify_call(Sema *sema, ASTNode *node, const char *fn_name) {
    if (node->call.callee->kind == NODE_CLOSURE) {
        return CALL_CLOSURE;
    }
    if (fn_name == NULL) {
        return CALL_EXPR;
    }
    if (BUF_LEN(node->call.type_args) > 0) {
        return CALL_GENERIC;
    }
    if (sema_lookup_fn(sema, fn_name) != NULL) {
        return CALL_FN;
    }
    if (infer_generic_call(sema, node, fn_name) != NULL) {
        // infer_generic_call already applied the type — return its kind directly.
        // Re-classify as FN_TYPE so the switch returns node->type.
        return CALL_FN_TYPE;
    }
    Sym *sym = scope_lookup(sema, fn_name);
    if (sym != NULL && (sym->kind == SYM_FN || (sym->type != NULL && sym->type->kind == TYPE_FN))) {
        return CALL_FN_TYPE;
    }
    if (check_tuple_struct_call(sema, node, fn_name) != NULL) {
        return CALL_TUPLE_CTOR;
    }
    if (sym == NULL && check_bare_variant_call(sema, node, fn_name) != NULL) {
        return CALL_BARE_VARIANT;
    }
    return CALL_UNKNOWN;
}

const Type *check_call(Sema *sema, ASTNode *node) {
    const char *fn_name = NULL;

    // Early dispatch for member calls and closures.
    if (node->call.callee->kind == NODE_ID) {
        fn_name = node->call.callee->id.name;
    } else if (node->call.callee->kind == NODE_MEMBER) {
        const Type *result = check_member_call(sema, node, &fn_name);
        if (result != NULL) {
            return result;
        }
    } else if (node->call.callee->kind == NODE_CLOSURE) {
        return check_inline_closure_call(sema, node);
    }

    check_call_args(sema, node, fn_name);

    // Phase 1: classify.
    CallKind kind = classify_call(sema, node, fn_name);

    // Phase 2: dispatch.
    switch (kind) {
    case CALL_CLOSURE:
        return check_inline_closure_call(sema, node);

    case CALL_METHOD:
        // Already handled above; unreachable here.
        break;

    case CALL_GENERIC: {
        // Try tuple struct constructor first for generic types.
        const Type *ts = check_tuple_struct_call(sema, node, fn_name);
        if (ts != NULL) {
            return ts;
        }
        return check_generic_fn_call(sema, node, fn_name);
    }

    case CALL_FN: {
        FnSig *sig = sema_lookup_fn(sema, fn_name);
        // If found via module-qualified fallback, rewrite callee name.
        if (sig != NULL && sema->base.current_scope->module_name != NULL &&
            hash_table_lookup(&sema->base.db.fn_table, fn_name) == NULL) {
            const char *qualified = arena_sprintf(sema->base.arena, "%s.%s",
                                                  sema->base.current_scope->module_name, fn_name);
            node->call.callee->id.name = qualified;
        }
        if (sig != NULL) {
            return resolve_call(sema, node, sig);
        }
        break;
    }

    case CALL_FN_TYPE: {
        // infer_generic_call may have already set node->type.
        if (node->type != NULL && node->type->kind != TYPE_ERR) {
            return node->type;
        }
        Sym *sym = scope_lookup(sema, fn_name);
        if (sym != NULL && sym->kind == SYM_FN) {
            return sym->type;
        }
        if (sym != NULL && sym->type != NULL && sym->type->kind == TYPE_FN) {
            return check_fn_type_call(sema, node, sym->type);
        }
        break;
    }

    case CALL_TUPLE_CTOR:
    case CALL_BARE_VARIANT:
        // Already applied by classify_call via
        // check_tuple_struct_call or check_bare_variant_call.
        return node->type;

    case CALL_EXPR: {
        const Type *ct = check_node(sema, node->call.callee);
        if (ct != NULL && ct->kind == TYPE_FN) {
            return check_fn_type_call(sema, node, ct);
        }
        break;
    }

    case CALL_UNKNOWN:
        SEMA_ERR(sema, node->loc, "undefined function '%s'", fn_name);
        break;
    }

    return &TYPE_ERR_INST;
}
