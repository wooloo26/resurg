#include "_check.h"

// ── Expression checkers ────────────────────────────────────────────────

const Type *check_lit(Sema *sema, ASTNode *node) {
    (void)sema;
    return lit_kind_to_type(node->lit.kind);
}

const Type *check_id(Sema *sema, ASTNode *node) {
    // Resolve Self to the enclosing type name in expression context
    if (strcmp(node->id.name, "Self") == 0 && sema->infer.self_type_name != NULL) {
        node->id.name = sema->infer.self_type_name;
    }

    const char *qualified = NULL;
    Sym *sym = scope_lookup_qualified(sema, node->id.name, &qualified);
    if (qualified != NULL) {
        node->id.name = qualified;
    }

    if (sym == NULL) {
        // Handle 'super' — resolve to parent module
        if (strcmp(node->id.name, "super") == 0 && sema->base.current_scope->module_name != NULL) {
            const char *cur = sema->base.current_scope->module_name;
            const char *last_dot = strrchr(cur, '.');
            if (last_dot != NULL) {
                // "a.b" → "a"
                size_t parent_len = (size_t)(last_dot - cur);
                char *parent = arena_alloc(sema->base.arena, parent_len + 1);
                memcpy(parent, cur, parent_len);
                parent[parent_len] = '\0';
                return type_create_module(sema->base.arena, parent);
            }
            // Top-level module: super goes to file scope (empty module name)
            return type_create_module(sema->base.arena, "");
        }
        // Handle 'self' — resolve to file scope
        if (strcmp(node->id.name, "self") == 0) {
            return type_create_module(sema->base.arena, "");
        }
        // Handle bare unit variant (e.g., None) — resolve from expected type or fn return type
        VariantCtorInfo *vci = hash_table_lookup(&sema->base.db.variant_ctor_table, node->id.name);
        if (vci != NULL && !vci->has_payload) {
            const Type *ctx = sema->infer.expected_type;
            if (ctx == NULL || ctx->kind != TYPE_ENUM) {
                ctx = sema->infer.fn_return_type;
            }
            if (ctx != NULL && ctx->kind == TYPE_ENUM) {
                const EnumVariant *variant = type_enum_find_variant(ctx, node->id.name);
                if (variant != NULL) {
                    ASTNode *vc =
                        build_unit_variant_call(sema->base.arena, ctx, node->id.name, node->loc);
                    node->kind = vc->kind;
                    node->call = vc->call;
                    node->type = ctx;
                    return ctx;
                }
            }
        }
        SEMA_ERR(sema, node->loc, "undefined variable '%s'", node->id.name);
        return &TYPE_ERR_INST;
    }
    // Track captured variable references for Fn/FnMut auto-inference
    if (sema->closure.scope != NULL && sym->kind != SYM_FN && sym->kind != SYM_TYPE) {
        bool found_local = false;
        for (Scope *s = sema->base.current_scope; s != NULL && s != sema->closure.scope->parent;
             s = s->parent) {
            if (hash_table_lookup(&s->table, node->id.name) != NULL) {
                found_local = true;
                break;
            }
        }
        if (!found_local) {
            sema->closure.has_capture = true;
            // Collect captured name (deduplicated) for persistence to AST
            bool already_recorded = false;
            for (int32_t ci = 0; ci < BUF_LEN(sema->closure.capture_names); ci++) {
                if (strcmp(sema->closure.capture_names[ci], node->id.name) == 0) {
                    already_recorded = true;
                    break;
                }
            }
            if (!already_recorded) {
                BUF_PUSH(sema->closure.capture_names, node->id.name);
            }
        }
    }
    // Function symbols used as values → construct fn type
    if (sym->kind == SYM_FN) {
        FnSig *sig = sema_lookup_fn(sema, node->id.name);
        if (sig != NULL) {
            FnTypeSpec fn_spec = {sig->param_types, sig->param_count, sig->return_type, FN_PLAIN};
            return type_create_fn(sema->base.arena, &fn_spec);
        }
    }
    return sym->type;
}

const Type *check_unary(Sema *sema, ASTNode *node) {
    const Type *operand = check_node(sema, node->unary.operand);
    if (operand == NULL || operand->kind == TYPE_ERR) {
        return &TYPE_ERR_INST;
    }
    if (node->unary.op == TOKEN_BANG) {
        if (!type_equal(operand, &TYPE_BOOL_INST)) {
            SEMA_ERR(sema, node->loc, "'!' requires 'bool' operand, got '%s'",
                     type_name(sema->base.arena, operand));
            return &TYPE_ERR_INST;
        }
        return &TYPE_BOOL_INST;
    }
    if (node->unary.op == TOKEN_MINUS) {
        if (!type_is_numeric(operand)) {
            SEMA_ERR(sema, node->loc, "'-' requires numeric operand, got '%s'",
                     type_name(sema->base.arena, operand));
            return &TYPE_ERR_INST;
        }
        return operand;
    }
    return operand;
}

const Type *check_binary(Sema *sema, ASTNode *node) {
    const Type *left = check_node(sema, node->binary.left);
    const Type *right = check_node(sema, node->binary.right);

    if (left == NULL || right == NULL || left->kind == TYPE_ERR || right->kind == TYPE_ERR) {
        return token_op_yields_bool(node->binary.op) ? &TYPE_BOOL_INST : &TYPE_ERR_INST;
    }

    // Promote integer/float lits to match the other side's type
    if (!type_equal(left, right)) {
        const Type *promoted;
        promoted = promote_lit(node->binary.left, right);
        if (promoted != NULL) {
            left = promoted;
        }
        promoted = promote_lit(node->binary.right, left);
        if (promoted != NULL) {
            right = promoted;
        }
    }

    // Check for type mismatch after promotion
    if (!type_equal(left, right)) {
        if (left->kind != TYPE_ERR && right->kind != TYPE_ERR) {
            SEMA_ERR(sema, node->loc, "type mismatch: '%s' and '%s'",
                     type_name(sema->base.arena, left), type_name(sema->base.arena, right));
        }
    }

    // Comparison and logical operators return bool; arithmetic returns operand type
    if (token_op_yields_bool(node->binary.op)) {
        return &TYPE_BOOL_INST;
    }
    return left;
}

const Type *check_type_conversion(Sema *sema, ASTNode *node) {
    check_node(sema, node->type_conversion.operand);
    const Type *target = resolve_ast_type(sema, &node->type_conversion.target_type);
    if (target == NULL) {
        return &TYPE_ERR_INST;
    }
    promote_lit(node->type_conversion.operand, target);
    return target;
}

const Type *check_str_interpolation(Sema *sema, ASTNode *node) {
    for (int32_t i = 0; i < BUF_LEN(node->str_interpolation.parts); i++) {
        check_node(sema, node->str_interpolation.parts[i]);
    }
    return &TYPE_STR_INST;
}

const Type *check_address_of(Sema *sema, ASTNode *node) {
    const Type *inner_type = check_node(sema, node->address_of.operand);
    if (inner_type == NULL || inner_type->kind == TYPE_ERR) {
        return &TYPE_ERR_INST;
    }
    // Addressability: vars, struct/slice/array lits are addressable
    ASTNode *operand = node->address_of.operand;
    if (operand->kind != NODE_ID && operand->kind != NODE_STRUCT_LIT &&
        operand->kind != NODE_SLICE_LIT && operand->kind != NODE_ARRAY_LIT &&
        operand->kind != NODE_MEMBER && operand->kind != NODE_IDX) {
        SEMA_ERR(sema, node->loc, "cannot take address of rvalue");
        return &TYPE_ERR_INST;
    }
    return type_create_ptr(sema->base.arena, inner_type, false);
}

const Type *check_deref(Sema *sema, ASTNode *node) {
    const Type *inner_type = check_node(sema, node->deref.operand);
    if (inner_type == NULL || inner_type->kind == TYPE_ERR) {
        return &TYPE_ERR_INST;
    }
    if (inner_type->kind != TYPE_PTR) {
        SEMA_ERR(sema, node->loc, "cannot deref non-ptr type '%s'",
                 type_name(sema->base.arena, inner_type));
        return &TYPE_ERR_INST;
    }
    return inner_type->ptr.pointee;
}

const Type *check_closure(Sema *sema, ASTNode *node) {
    const Type *expected = sema->infer.expected_type;
    int32_t param_count = BUF_LEN(node->closure.params);
    const Type **param_types = NULL;

    // Determine fn_kind from expected type (NULL means auto-infer)
    FnTypeKind fn_kind = FN_PLAIN;
    bool infer_fn_kind = (expected == NULL || expected->kind != TYPE_FN);
    if (!infer_fn_kind) {
        fn_kind = expected->fn_type.fn_kind;
    }

    for (int32_t i = 0; i < param_count; i++) {
        ASTNode *param = node->closure.params[i];
        const Type *pt = resolve_ast_type(sema, &param->param.type);
        if (pt == NULL && expected != NULL && expected->kind == TYPE_FN &&
            i < expected->fn_type.param_count) {
            pt = expected->fn_type.params[i];
        }
        if (pt == NULL) {
            pt = &TYPE_ERR_INST;
        }
        param->type = pt;
        BUF_PUSH(param_types, pt);
    }

    scope_push(sema, false);
    for (int32_t i = 0; i < param_count; i++) {
        scope_define(
            sema, &(SymDef){node->closure.params[i]->param.name, param_types[i], false, SYM_VAR});
    }

    // Save and set closure context for capture mutation checking
    ClosureCtx saved_closure = sema->closure;

    // Always set up closure scope so capture tracking works
    sema->closure.scope = sema->base.current_scope;
    sema->closure.fn_kind = infer_fn_kind ? FN_CLOSURE_MUT : fn_kind;
    sema->closure.capture_names = NULL;
    if (infer_fn_kind) {
        sema->closure.has_capture = false;
        sema->closure.captures_mutated = false;
    }

    const Type *body_type = check_node(sema, node->closure.body);

    // Persist capture names to AST so the lower pass can reuse them
    node->closure.capture_names = sema->closure.capture_names;

    // Auto-infer fn_kind from capture analysis
    if (infer_fn_kind) {
        if (sema->closure.captures_mutated) {
            fn_kind = FN_CLOSURE_MUT;
        } else if (sema->closure.has_capture) {
            fn_kind = FN_CLOSURE;
        }
    }

    sema->closure = saved_closure;
    scope_pop(sema);

    const Type *return_type = resolve_ast_type(sema, &node->closure.return_type);
    if (return_type == NULL) {
        return_type = (body_type != NULL) ? body_type : &TYPE_UNIT_INST;
    }

    FnTypeSpec fn_spec = {param_types, param_count, return_type, fn_kind};
    return type_create_fn(sema->base.arena, &fn_spec);
}
