#include "_check.h"

// ── Reserved-prefix validation ─────────────────────────────────────────

/**
 * Return true (and emit a diagnostic) if @p name starts with a prefix
 * reserved for the pipeline or runtime.  Checked prefixes:
 *
 *   rsg_   – runtime fns / types
 *   rsgu_  – codegen-mangled user fns
 *   _rsg   – codegen temporaries and internal vars
 *   _Rsg   – codegen compound-type names
 */
static bool is_reserved_id(Sema *sema, SrcLoc loc, const char *name) {
    bool reserved = strncmp(name, "rsg_", 4) == 0 || strncmp(name, "rsgu_", 5) == 0 ||
                    strncmp(name, "_rsg", 4) == 0 || strncmp(name, "_Rsg", 4) == 0;
    if (reserved) {
        SEMA_ERR(sema, loc, "identifier '%s' uses a prefix reserved for the runtime", name);
    }
    return reserved;
}

// ── Statement / decl checkers ───────────────────────────────────

const Type *check_if(Sema *sema, ASTNode *node) {
    if (node->if_expr.pattern != NULL) {
        // if-let pattern binding: if Some(x) := expr { ... }
        const Type *init_type = check_node(sema, node->if_expr.pattern_init);

        // Push a scope for pattern bindings visible inside then_body
        scope_push(sema, false);
        if (init_type != NULL && init_type->kind == TYPE_ENUM) {
            // Use the full check_pattern from check_match with dummy tracking
            int32_t vc = type_enum_variant_count(init_type);
            bool *variant_covered = arena_alloc_zero(sema->arena, vc * sizeof(bool));
            bool has_wildcard = false;
            check_pattern(sema, node->if_expr.pattern, init_type,
                          &(MatchCoverage){variant_covered, &has_wildcard});
        }
        const Type *then_type = check_node(sema, node->if_expr.then_body);
        scope_pop(sema);

        const Type *else_type = NULL;
        if (node->if_expr.else_body != NULL) {
            else_type = check_node(sema, node->if_expr.else_body);
        }

        if (then_type != NULL && then_type->kind == TYPE_NEVER && else_type != NULL) {
            return else_type;
        }
        if (else_type != NULL && else_type->kind == TYPE_NEVER && then_type != NULL) {
            return then_type;
        }
        if (else_type != NULL && then_type != NULL && then_type->kind != TYPE_UNIT) {
            return then_type;
        }
        return &TYPE_UNIT_INST;
    }

    check_node(sema, node->if_expr.cond);
    const Type *then_type = check_node(sema, node->if_expr.then_body);
    const Type *else_type = NULL;
    if (node->if_expr.else_body != NULL) {
        else_type = check_node(sema, node->if_expr.else_body);
    }

    // Never-type coercion: if one branch is never, use the other
    if (then_type != NULL && then_type->kind == TYPE_NEVER && else_type != NULL) {
        return else_type;
    }
    if (else_type != NULL && else_type->kind == TYPE_NEVER && then_type != NULL) {
        return then_type;
    }

    // If both branches present and non-unit, return their common type
    if (else_type != NULL && then_type != NULL && then_type->kind != TYPE_UNIT) {
        return then_type;
    }
    if (else_type != NULL && else_type->kind != TYPE_UNIT) {
        return else_type;
    }
    if (then_type != NULL) {
        return then_type;
    }
    return &TYPE_UNIT_INST;
}

const Type *check_block(Sema *sema, ASTNode *node) {
    scope_push(sema, false);
    const Type *last_stmt_type = NULL;
    for (int32_t i = 0; i < BUF_LEN(node->block.stmts); i++) {
        last_stmt_type = check_node(sema, node->block.stmts[i]);
    }
    const Type *result_type = &TYPE_UNIT_INST;
    if (node->block.result != NULL) {
        result_type = check_node(sema, node->block.result);
    } else if (last_stmt_type != NULL && last_stmt_type->kind == TYPE_NEVER) {
        // Block whose last stmt diverges (return/break/continue) is never.
        result_type = &TYPE_NEVER_INST;
    }
    scope_pop(sema);
    return result_type;
}

/** Promote array lit elem types to match the declared elem type. */
static bool promote_array_elem_lits(ASTNode *init, const Type *declared) {
    for (int32_t i = 0; i < BUF_LEN(init->array_lit.elems); i++) {
        ASTNode *elem = init->array_lit.elems[i];
        if (promote_lit(elem, declared->array.elem) == NULL && elem->type != NULL &&
            !type_equal(elem->type, declared->array.elem)) {
            return false;
        }
    }
    return true;
}

/**
 * Reconcile declared type with init expression: promote literals, check
 * array element types, and diagnose mismatches. Returns the resolved var type.
 */
static const Type *reconcile_var_init(Sema *sema, ASTNode *node, const Type *declared,
                                      const Type *init_type) {
    if (init_type == NULL && node->var_decl.init == NULL && declared->kind == TYPE_ENUM &&
        type_enum_find_variant(declared, "None") != NULL) {
        node->var_decl.init = build_none_variant_call(sema->arena, declared, node->loc);
        return declared;
    }
    if (init_type != NULL && node->var_decl.init != NULL) {
        promote_lit(node->var_decl.init, declared);

        ASTNode *init = node->var_decl.init;
        bool is_array_mismatch = init->kind == NODE_ARRAY_LIT && declared->kind == TYPE_ARRAY &&
                                 init_type->kind == TYPE_ARRAY && !type_equal(declared, init_type);
        if (is_array_mismatch) {
            if (promote_array_elem_lits(init, declared)) {
                init->type = declared;
            }
        }
        init_type = node->var_decl.init->type;
    }
    if (init_type != NULL && !type_assignable(init_type, declared) && init_type->kind != TYPE_ERR &&
        declared->kind != TYPE_ERR) {
        SEMA_ERR(sema, node->loc, "type mismatch: expected '%s', got '%s'",
                 type_name(sema->arena, declared), type_name(sema->arena, init_type));
    }
    return declared;
}

const Type *check_var_decl(Sema *sema, ASTNode *node) {
    const Type *declared = resolve_ast_type(sema, &node->var_decl.type);

    // Set expected type before checking init for bidirectional inference
    const Type *save_expected = sema->expected_type;
    if (declared != NULL) {
        sema->expected_type = declared;
    }

    const Type *init_type = NULL;
    if (node->var_decl.init != NULL) {
        init_type = check_node(sema, node->var_decl.init);
    }

    sema->expected_type = save_expected;

    // Determine final type
    const Type *var_type;
    if (declared != NULL) {
        var_type = reconcile_var_init(sema, node, declared, init_type);
    } else if (init_type != NULL) {
        var_type = init_type;
    } else {
        SEMA_ERR(sema, node->loc, "cannot infer type for '%s'", node->var_decl.name);
        var_type = &TYPE_ERR_INST;
    }

    if (scope_lookup_current(sema, node->var_decl.name) != NULL) {
        // Same-scope rebinding is allowed (shadowing)
    }

    is_reserved_id(sema, node->loc, node->var_decl.name);

    scope_define(sema, &(SymDef){node->var_decl.name, var_type, false, SYM_VAR});

    // Mark immutable bindings
    if (node->var_decl.is_immut) {
        Sym *sym = scope_lookup_current(sema, node->var_decl.name);
        if (sym != NULL) {
            sym->is_immut = true;
        }
    }

    return var_type;
}

void check_fn_body(Sema *sema, ASTNode *fn_node) {
    is_reserved_id(sema, fn_node->loc, fn_node->fn_decl.name);

    scope_push(sema, false);

    // Register params
    for (int32_t i = 0; i < BUF_LEN(fn_node->fn_decl.params); i++) {
        ASTNode *param = fn_node->fn_decl.params[i];
        const Type *param_type = resolve_ast_type(sema, &param->param.type);
        if (param_type == NULL) {
            param_type = &TYPE_ERR_INST;
        }
        param->type = param_type;
        is_reserved_id(sema, param->loc, param->param.name);
        scope_define(sema, &(SymDef){param->param.name, param_type, false, SYM_PARAM});
    }

    // Check body
    if (fn_node->fn_decl.body != NULL) {
        // Pre-resolve return type for bidirectional inference (Ok/Err/None)
        const Type *pre_return = resolve_ast_type(sema, &fn_node->fn_decl.return_type);
        const Type *save_fn_return = sema->fn_return_type;
        const Type *save_expected = sema->expected_type;
        if (pre_return != NULL) {
            sema->fn_return_type = pre_return;
            sema->expected_type = pre_return;
        }

        const Type *body_type = check_node(sema, fn_node->fn_decl.body);

        sema->fn_return_type = save_fn_return;
        sema->expected_type = save_expected;

        // If return type not declared, infer from body
        const Type *resolved_return = pre_return;
        if (resolved_return == NULL) {
            resolved_return = body_type != NULL ? body_type : &TYPE_UNIT_INST;
        }
        fn_node->type = resolved_return;

        // Update the fn's sym type to the resolved return type
        Sym *sym = scope_lookup(sema, fn_node->fn_decl.name);
        if (sym != NULL) {
            sym->type = resolved_return;
        }

        // Update fn sigs
        const char *sig_key = fn_node->fn_decl.name;
        if (fn_node->fn_decl.owner_struct != NULL) {
            sig_key = arena_sprintf(sema->arena, "%s.%s", fn_node->fn_decl.owner_struct, sig_key);
        }
        FnSig *sig = sema_lookup_fn(sema, sig_key);
        if (sig != NULL && sig->return_type->kind == TYPE_UNIT) {
            sig->return_type = resolved_return;
        }
    }

    scope_pop(sema);
}

/** Shared logic for simple and compound assignment type-checking. */
static const Type *check_assignment_common(Sema *sema, ASTNode *target, ASTNode *value) {
    const Type *target_type = check_node(sema, target);

    const Type *saved_expected = sema->expected_type;
    sema->expected_type = target_type;
    check_node(sema, value);
    sema->expected_type = saved_expected;

    promote_lit(value, target_type);

    // Check immutability: cannot assign to immut bindings
    if (target->kind == NODE_ID) {
        Sym *sym = scope_lookup(sema, target->id.name);
        if (sym != NULL && sym->is_immut) {
            SEMA_ERR(sema, target->loc, "cannot assign to immutable var '%s'", target->id.name);
        }
        // Check Fn closure: cannot mutate captured variables
        if (sema->closure_scope != NULL &&
            (sema->closure_fn_kind == FN_CLOSURE || sema->closure_fn_kind == FN_CLOSURE_MUT)) {
            const char *name = target->id.name;
            // Search only within the closure scope (closure params + body locals)
            bool found_local = false;
            for (Scope *s = sema->current_scope; s != NULL && s != sema->closure_scope->parent;
                 s = s->parent) {
                if (hash_table_lookup(&s->table, name) != NULL) {
                    found_local = true;
                    break;
                }
            }
            if (!found_local) {
                if (sema->closure_fn_kind == FN_CLOSURE) {
                    SEMA_ERR(sema, target->loc,
                             "cannot assign mutable closure to Fn: "
                             "captured variable '%s' is mutated",
                             name);
                } else {
                    sema->closure_captures_mutated = true;
                }
            }
        }
    }

    return &TYPE_UNIT_INST;
}

const Type *check_assign(Sema *sema, ASTNode *node) {
    return check_assignment_common(sema, node->assign.target, node->assign.value);
}

const Type *check_compound_assign(Sema *sema, ASTNode *node) {
    return check_assignment_common(sema, node->compound_assign.target, node->compound_assign.value);
}

static const Type *check_loop(Sema *sema, ASTNode *node) {
    const Type *saved_break_type = sema->loop_break_type;
    sema->loop_break_type = NULL;
    scope_push(sema, true);
    check_node(sema, node->loop.body);
    scope_pop(sema);
    const Type *result = sema->loop_break_type;
    sema->loop_break_type = saved_break_type;
    return (result != NULL) ? result : &TYPE_NEVER_INST;
}

static void check_while(Sema *sema, ASTNode *node) {
    if (node->while_loop.pattern != NULL) {
        // while-let pattern binding: while Some(x) := expr { ... }
        const Type *init_type = check_node(sema, node->while_loop.pattern_init);
        scope_push(sema, true);
        if (init_type != NULL && init_type->kind == TYPE_ENUM) {
            int32_t vc = type_enum_variant_count(init_type);
            bool *variant_covered = arena_alloc_zero(sema->arena, vc * sizeof(bool));
            bool has_wildcard = false;
            check_pattern(sema, node->while_loop.pattern, init_type,
                          &(MatchCoverage){variant_covered, &has_wildcard});
        }
        check_node(sema, node->while_loop.body);
        scope_pop(sema);
        return;
    }

    const Type *cond_type = check_node(sema, node->while_loop.cond);
    if (cond_type != NULL && cond_type->kind != TYPE_BOOL && cond_type->kind != TYPE_ERR) {
        SEMA_ERR(sema, node->while_loop.cond->loc, "condition must be 'bool', got '%s'",
                 type_name(sema->arena, cond_type));
    }
    scope_push(sema, true);
    check_node(sema, node->while_loop.body);
    scope_pop(sema);
}

static void check_defer(Sema *sema, ASTNode *node) {
    check_node(sema, node->defer_stmt.body);
}

static void check_for(Sema *sema, ASTNode *node) {
    if (node->for_loop.iterable != NULL) {
        // Slice iteration: for slice |v| or for slice |v, i|
        const Type *iter_type = check_node(sema, node->for_loop.iterable);
        scope_push(sema, true);
        const Type *elem_type = &TYPE_ERR_INST;
        if (iter_type != NULL && iter_type->kind == TYPE_SLICE) {
            elem_type = iter_type->slice.elem;
        }
        is_reserved_id(sema, node->loc, node->for_loop.var_name);
        scope_define(sema, &(SymDef){node->for_loop.var_name, elem_type, false, SYM_VAR});
        if (node->for_loop.idx_name != NULL) {
            is_reserved_id(sema, node->loc, node->for_loop.idx_name);
            scope_define(sema, &(SymDef){node->for_loop.idx_name, &TYPE_I32_INST, false, SYM_VAR});
        }
        check_node(sema, node->for_loop.body);
        scope_pop(sema);
    } else {
        // Range iteration: for start..end |i|
        check_node(sema, node->for_loop.start);
        check_node(sema, node->for_loop.end);
        scope_push(sema, true);
        is_reserved_id(sema, node->loc, node->for_loop.var_name);
        scope_define(sema, &(SymDef){node->for_loop.var_name, &TYPE_I32_INST, false, SYM_VAR});
        check_node(sema, node->for_loop.body);
        scope_pop(sema);
    }
}

static void check_break_continue(Sema *sema, ASTNode *node) {
    if (!in_loop(sema)) {
        SEMA_ERR(sema, node->loc, "'%s' outside of loop",
                 node->kind == NODE_BREAK ? "break" : "continue");
    }
    if (node->kind == NODE_BREAK && node->break_stmt.value != NULL) {
        const Type *val_type = check_node(sema, node->break_stmt.value);
        if (sema->loop_break_type == NULL) {
            sema->loop_break_type = val_type;
        }
    }
}

// ── Node dispatch ──────────────────────────────────────────────────────

/** Check `expr?.member` — optional chaining on Option types. */
static const Type *check_optional_chain(Sema *sema, ASTNode *node) {
    const Type *obj_type = check_node(sema, node->optional_chain.object);
    if (obj_type == NULL || obj_type->kind == TYPE_ERR) {
        return NULL;
    }
    if (obj_type->kind != TYPE_ENUM) {
        SEMA_ERR(sema, node->loc, "optional chaining requires Option type, got '%s'",
                 type_name(sema->arena, obj_type));
        return NULL;
    }
    const EnumVariant *some_var = type_enum_find_variant(obj_type, "Some");
    if (some_var == NULL || some_var->kind != ENUM_VARIANT_TUPLE || some_var->tuple_count != 1) {
        SEMA_ERR(sema, node->loc, "optional chaining requires Option type");
        return NULL;
    }
    const Type *inner = some_var->tuple_types[0];
    if (inner->kind == TYPE_PTR) {
        inner = inner->ptr.pointee;
    }
    if (inner->kind != TYPE_STRUCT) {
        return NULL;
    }
    const StructField *field = type_struct_find_field(inner, node->optional_chain.member);
    if (field == NULL) {
        return NULL;
    }
    const Type *field_type = field->type;
    // Already Option — don't double-wrap
    if (field_type->kind == TYPE_ENUM && type_enum_find_variant(field_type, "Some") != NULL) {
        return field_type;
    }
    // Instantiate Option<field_type>
    GenericEnumDef *gdef = sema_lookup_generic_enum(sema, "Option");
    if (gdef == NULL) {
        return NULL;
    }
    ASTType val_arg = {.kind = AST_TYPE_NAME,
                       .name = type_name(sema->arena, field_type),
                       .type_args = NULL,
                       .loc = node->loc};
    hash_table_insert(&sema->type_param_table, val_arg.name, (void *)field_type);
    GenericInstArgs inst_args = {&val_arg, 1, node->loc};
    const char *mangled = instantiate_generic_enum(sema, gdef, &inst_args);
    hash_table_remove(&sema->type_param_table, val_arg.name);
    if (mangled != NULL) {
        return sema_lookup_type_alias(sema, mangled);
    }
    return NULL;
}

/** Check `expr!` — unwrap Result<T, E>, propagate Err. */
static const Type *check_try(Sema *sema, ASTNode *node) {
    const Type *operand_type = check_node(sema, node->try_expr.operand);
    if (operand_type == NULL || operand_type->kind == TYPE_ERR) {
        return NULL;
    }
    if (operand_type->kind != TYPE_ENUM) {
        SEMA_ERR(sema, node->loc, "postfix '!' requires Result type, got '%s'",
                 type_name(sema->arena, operand_type));
        return NULL;
    }
    const EnumVariant *ok_var = type_enum_find_variant(operand_type, "Ok");
    if (ok_var == NULL || ok_var->kind != ENUM_VARIANT_TUPLE || ok_var->tuple_count != 1) {
        SEMA_ERR(sema, node->loc, "postfix '!' requires Result type with Ok variant");
        return NULL;
    }
    return ok_var->tuple_types[0];
}

const Type *check_node(Sema *sema, ASTNode *node) {
    if (node == NULL) {
        return &TYPE_UNIT_INST;
    }
    const Type *result = &TYPE_UNIT_INST;

    switch (node->kind) {
    case NODE_FILE:
        for (int32_t i = 0; i < BUF_LEN(node->file.decls); i++) {
            check_node(sema, node->file.decls[i]);
        }
        break;

    case NODE_MODULE:
        sema->current_scope->module_name = node->module.name;
        break;

    case NODE_TYPE_ALIAS:
        // Already processed in first pass
        break;

    case NODE_FN_DECL:
        // Skip generic fn templates — they are checked per-instantiation
        if (BUF_LEN(node->fn_decl.type_params) > 0) {
            break;
        }
        check_fn_body(sema, node);
        result = node->type; // preserve type set by check_fn_body
        break;

    case NODE_VAR_DECL:
        result = check_var_decl(sema, node);
        break;

    case NODE_PARAM:
        result = resolve_ast_type(sema, &node->param.type);
        if (result == NULL) {
            result = &TYPE_ERR_INST;
        }
        break;

    case NODE_EXPR_STMT:
        check_node(sema, node->expr_stmt.expr);
        break;

    case NODE_BREAK:
    case NODE_CONTINUE:
        check_break_continue(sema, node);
        result = &TYPE_NEVER_INST;
        break;

    case NODE_LIT:
        result = check_lit(sema, node);
        break;

    case NODE_ID:
        result = check_id(sema, node);
        break;

    case NODE_UNARY:
        result = check_unary(sema, node);
        break;

    case NODE_BINARY:
        result = check_binary(sema, node);
        break;

    case NODE_ASSIGN:
        result = check_assign(sema, node);
        break;

    case NODE_COMPOUND_ASSIGN:
        result = check_compound_assign(sema, node);
        break;

    case NODE_CALL:
        result = check_call(sema, node);
        break;

    case NODE_MEMBER:
        result = check_member(sema, node);
        break;

    case NODE_IDX:
        result = check_idx(sema, node);
        break;

    case NODE_IF:
        result = check_if(sema, node);
        break;

    case NODE_LOOP:
        result = check_loop(sema, node);
        break;

    case NODE_FOR:
        check_for(sema, node);
        break;

    case NODE_BLOCK:
        result = check_block(sema, node);
        break;

    case NODE_STR_INTERPOLATION:
        result = check_str_interpolation(sema, node);
        break;

    case NODE_ARRAY_LIT:
        result = check_array_lit(sema, node);
        break;

    case NODE_SLICE_LIT:
        result = check_slice_lit(sema, node);
        break;

    case NODE_SLICE_EXPR:
        result = check_slice_expr(sema, node);
        break;

    case NODE_TUPLE_LIT:
        result = check_tuple_lit(sema, node);
        break;

    case NODE_TYPE_CONVERSION:
        result = check_type_conversion(sema, node);
        break;

    case NODE_STRUCT_DECL:
        result = check_struct_decl(sema, node);
        break;

    case NODE_PACT_DECL:
        result = check_pact_decl(sema, node);
        break;

    case NODE_STRUCT_LIT:
        result = check_struct_lit(sema, node);
        break;

    case NODE_STRUCT_DESTRUCTURE:
        result = check_struct_destructure(sema, node);
        break;

    case NODE_TUPLE_DESTRUCTURE:
        result = check_tuple_destructure(sema, node);
        break;

    case NODE_ADDRESS_OF:
        result = check_address_of(sema, node);
        break;

    case NODE_DEREF:
        result = check_deref(sema, node);
        break;

    case NODE_CLOSURE:
        result = check_closure(sema, node);
        break;

    case NODE_ENUM_DECL:
        result = check_enum_decl_body(sema, node);
        break;

    case NODE_RETURN:
        if (node->return_stmt.value != NULL) {
            const Type *save_expected = sema->expected_type;
            sema->expected_type = sema->fn_return_type;
            check_node(sema, node->return_stmt.value);
            sema->expected_type = save_expected;
        }
        result = &TYPE_NEVER_INST;
        break;

    case NODE_WHILE:
        check_while(sema, node);
        break;

    case NODE_DEFER:
        check_defer(sema, node);
        break;

    case NODE_MATCH:
        result = check_match(sema, node);
        break;

    case NODE_ENUM_INIT:
        result = check_enum_init(sema, node);
        break;

    case NODE_OPTIONAL_CHAIN: {
        const Type *chain = check_optional_chain(sema, node);
        if (chain != NULL) {
            result = chain;
        }
        break;
    }

    case NODE_TRY: {
        const Type *try_type = check_try(sema, node);
        if (try_type != NULL) {
            result = try_type;
        }
        break;
    }
    }

    node->type = result;
    return result;
}
