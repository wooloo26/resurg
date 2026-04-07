#include "_check.h"

/**
 * @file check_call.c
 * @brief Call-expression type checking — fn calls, method dispatch, generics.
 *
 * Handles direct fn calls, member method calls, generic fn instantiation,
 * type inference, enum variant constructors, closure calls, and named args.
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
    ASTNode **reordered = NULL;
    for (int32_t i = 0; i < sig->param_count; i++) {
        BUF_PUSH(reordered, (ASTNode *)NULL);
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
        }
    }
    node->call.args = reordered;
    node->call.arg_names = NULL;
}

/**
 * Validate arg count against @p sig, promote lit args, and type-check each arg.
 */
static void check_and_promote_call_args(Sema *sema, ASTNode *node, const FnSig *sig) {
    int32_t arg_count = BUF_LEN(node->call.args);
    if (arg_count != sig->param_count) {
        SEMA_ERR(sema, node->loc, "expected %d args, got %d", sig->param_count, arg_count);
        return;
    }
    for (int32_t i = 0; i < arg_count; i++) {
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
                     type_name(sema->arena, param_type), type_name(sema->arena, arg_type));
        }
    }
}

// ── Enum variant call ─────────────────────────────────────────────

/** Check an enum tuple variant construction: Enum.Variant(args). */
static const Type *check_enum_variant_call(Sema *sema, ASTNode *node, const Type *enum_type,
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
                         type_name(sema->arena, variant->tuple_types[i]),
                         type_name(sema->arena, arg_type));
            }
        }
    }
    node->type = enum_type;
    return enum_type;
}

// ── Resolved call application ─────────────────────────────────────

/** Reorder named args, validate, and apply a resolved fn sig to a call. */
static const Type *resolve_call(Sema *sema, ASTNode *node, const FnSig *sig) {
    reorder_named_args(sema, node, sig);
    check_and_promote_call_args(sema, node, sig);
    node->type = sig->return_type;
    return sig->return_type;
}

// ── Method call resolution ────────────────────────────────────────

/** Resolve a struct method call, including promoted methods from embedded structs. */
static const Type *check_struct_method_call(Sema *sema, ASTNode *node, const Type *struct_type,
                                            const char *method_name) {
    const char *method_key =
        arena_sprintf(sema->arena, "%s.%s", struct_type->struct_type.name, method_name);
    FnSig *sig = sema_lookup_fn(sema, method_key);

    // If not found directly, check embedded structs for promoted methods
    if (sig == NULL) {
        StructDef *sdef = sema_lookup_struct(sema, struct_type->struct_type.name);
        if (sdef != NULL) {
            for (int32_t ei = 0; ei < BUF_LEN(sdef->embedded); ei++) {
                const char *embed_key =
                    arena_sprintf(sema->arena, "%s.%s", sdef->embedded[ei], method_name);
                sig = sema_lookup_fn(sema, embed_key);
                if (sig != NULL) {
                    break;
                }
            }
        }
    }
    if (sig == NULL) {
        return NULL;
    }
    for (int32_t i = 0; i < BUF_LEN(node->call.args); i++) {
        check_node(sema, node->call.args[i]);
    }
    return resolve_call(sema, node, sig);
}

/** Try to resolve a member call (enum variant, enum method, or struct method). */
static const Type *check_member_call(Sema *sema, ASTNode *node, const char **out_fn_name) {
    const char *method_name = node->call.callee->member.member;
    const Type *obj_type = NULL;

    // Handle generic enum instantiation via type_args on the call node.
    // Check BEFORE calling check_node to avoid spurious "undefined variable" errors.
    if (node->call.callee->member.object->kind == NODE_ID && BUF_LEN(node->call.type_args) > 0) {
        const char *enum_name = node->call.callee->member.object->id.name;
        GenericEnumDef *gdef = sema_lookup_generic_enum(sema, enum_name);
        if (gdef != NULL) {
            GenericInstArgs inst_args = {node->call.type_args, BUF_LEN(node->call.type_args),
                                         node->loc};
            const char *mangled = instantiate_generic_enum(sema, gdef, &inst_args);
            if (mangled != NULL) {
                node->call.callee->member.object->id.name = mangled;
                EnumDef *edef = sema_lookup_enum(sema, mangled);
                obj_type = edef->type;
                node->call.callee->member.object->type = obj_type;
                // Clear type_args since they've been consumed for enum instantiation
                node->call.type_args = NULL;
            }
        }
    }

    if (obj_type == NULL) {
        obj_type = check_node(sema, node->call.callee->member.object);
    }

    // Auto-deref for ptr types
    if (obj_type != NULL && obj_type->kind == TYPE_PTR) {
        obj_type = obj_type->ptr.pointee;
    }

    if (obj_type != NULL && obj_type->kind == TYPE_ENUM) {
        const Type *result = check_enum_variant_call(sema, node, obj_type, method_name);
        if (result != NULL) {
            return result;
        }
        // Enum method call
        const char *method_key =
            arena_sprintf(sema->arena, "%s.%s", type_enum_name(obj_type), method_name);
        FnSig *sig = sema_lookup_fn(sema, method_key);
        if (sig != NULL) {
            return resolve_call(sema, node, sig);
        }
    }

    if (obj_type != NULL && obj_type->kind == TYPE_STRUCT) {
        const Type *result = check_struct_method_call(sema, node, obj_type, method_name);
        if (result != NULL) {
            return result;
        }
    }

    // Call through fn-typed struct field: obj.field(args)
    if (obj_type != NULL && obj_type->kind == TYPE_STRUCT) {
        const StructField *sf = type_struct_find_field(obj_type, method_name);
        if (sf != NULL && sf->type != NULL && sf->type->kind == TYPE_FN) {
            node->call.callee->type = sf->type;
            int32_t arg_count = BUF_LEN(node->call.args);
            int32_t fn_param_count = sf->type->fn_type.param_count;
            if (arg_count != fn_param_count) {
                SEMA_ERR(sema, node->loc, "expected %d args, got %d", fn_param_count, arg_count);
            } else {
                for (int32_t i = 0; i < arg_count; i++) {
                    const Type *param_type = sf->type->fn_type.params[i];
                    promote_lit(node->call.args[i], param_type);
                    const Type *arg_type = node->call.args[i]->type;
                    if (arg_type != NULL && param_type != NULL &&
                        !type_assignable(arg_type, param_type) && arg_type->kind != TYPE_ERR &&
                        param_type->kind != TYPE_ERR) {
                        SEMA_ERR(
                            sema, node->call.args[i]->loc, "type mismatch: expected '%s', got '%s'",
                            type_name(sema->arena, param_type), type_name(sema->arena, arg_type));
                    }
                }
            }
            return sf->type->fn_type.return_type;
        }
    }

    *out_fn_name = method_name;
    return NULL;
}

// ── Main call dispatch ────────────────────────────────────────────

const Type *check_call(Sema *sema, ASTNode *node) {
    const char *fn_name = NULL;
    if (node->call.callee->kind == NODE_ID) {
        fn_name = node->call.callee->id.name;
    } else if (node->call.callee->kind == NODE_MEMBER) {
        const Type *result = check_member_call(sema, node, &fn_name);
        if (result != NULL) {
            return result;
        }
    } else if (node->call.callee->kind == NODE_CLOSURE) {
        // Inline closure call: (|x| expr)(args)
        for (int32_t i = 0; i < BUF_LEN(node->call.args); i++) {
            check_node(sema, node->call.args[i]);
        }
        // Infer closure param types from arg types
        int32_t param_count = BUF_LEN(node->call.callee->closure.params);
        int32_t arg_count = BUF_LEN(node->call.args);
        const Type **param_types = NULL;
        for (int32_t i = 0; i < param_count; i++) {
            const Type *pt = (i < arg_count && node->call.args[i]->type != NULL)
                                 ? node->call.args[i]->type
                                 : &TYPE_ERR_INST;
            BUF_PUSH(param_types, pt);
        }
        FnTypeSpec fn_spec = {param_types, param_count, NULL, FN_PLAIN};
        const Type *expected_fn = type_create_fn(sema->arena, &fn_spec);
        const Type *saved = sema->expected_type;
        sema->expected_type = expected_fn;
        const Type *callee_type = check_closure(sema, node->call.callee);
        sema->expected_type = saved;
        node->call.callee->type = callee_type;
        if (callee_type != NULL && callee_type->kind == TYPE_FN) {
            return callee_type->fn_type.return_type;
        }
        return &TYPE_ERR_INST;
    }

    // Check args — set expected_type per-param so closures can infer types
    FnSig *early_sig = (fn_name != NULL) ? sema_lookup_fn(sema, fn_name) : NULL;
    const Type *callee_fn_type = NULL;
    if (early_sig == NULL && fn_name != NULL) {
        Sym *callee_sym = scope_lookup(sema, fn_name);
        if (callee_sym != NULL && callee_sym->type != NULL && callee_sym->type->kind == TYPE_FN) {
            callee_fn_type = callee_sym->type;
        }
    }
    for (int32_t i = 0; i < BUF_LEN(node->call.args); i++) {
        const Type *saved = sema->expected_type;
        if (early_sig != NULL && i < early_sig->param_count) {
            sema->expected_type = early_sig->param_types[i];
        } else if (callee_fn_type != NULL && i < callee_fn_type->fn_type.param_count) {
            sema->expected_type = callee_fn_type->fn_type.params[i];
        }
        check_node(sema, node->call.args[i]);
        sema->expected_type = saved;
    }

    // Built-in fns
    if (fn_name != NULL && strcmp(fn_name, "assert") == 0) {
        return &TYPE_UNIT_INST;
    }
    if (fn_name != NULL && (strcmp(fn_name, "print") == 0 || strcmp(fn_name, "println") == 0)) {
        return &TYPE_UNIT_INST;
    }

    // Generic call handling: fn_name<Type, ...>(args)
    if (fn_name != NULL && BUF_LEN(node->call.type_args) > 0) {
        GenericFnDef *gdef = sema_lookup_generic_fn(sema, fn_name);
        if (gdef == NULL) {
            SEMA_ERR(sema, node->loc, "undefined generic function '%s'", fn_name);
            return &TYPE_ERR_INST;
        }
        int32_t expected = gdef->type_param_count;
        int32_t got = BUF_LEN(node->call.type_args);
        if (got != expected) {
            SEMA_ERR(sema, node->loc,
                     "wrong number of type arguments for '%s': expected %d, got %d", fn_name,
                     expected, got);
            return &TYPE_ERR_INST;
        }

        // Resolve type args
        const Type **resolved_args = NULL;
        for (int32_t i = 0; i < got; i++) {
            const Type *t = resolve_ast_type(sema, &node->call.type_args[i]);
            if (t == NULL) {
                t = &TYPE_ERR_INST;
            }
            BUF_PUSH(resolved_args, t);
        }

        // Check bounds
        for (int32_t i = 0; i < expected; i++) {
            for (int32_t b = 0; b < BUF_LEN(gdef->type_params[i].bounds); b++) {
                const char *bound = gdef->type_params[i].bounds[b];
                if (!type_satisfies_bound(sema, resolved_args[i], bound)) {
                    SEMA_ERR(sema, node->loc, "'%s' does not satisfy pact bound '%s'",
                             type_name(sema->arena, resolved_args[i]), bound);
                    return &TYPE_ERR_INST;
                }
            }
        }

        // Build mangled name
        const char *mangled = build_mangled_name(sema, fn_name, resolved_args, got);

        // Check if already instantiated
        FnSig *existing = sema_lookup_fn(sema, mangled);
        if (existing != NULL) {
            node->call.callee->id.name = mangled;
            return resolve_call(sema, node, existing);
        }

        // Push type param substitutions to resolve param/return types
        for (int32_t i = 0; i < expected; i++) {
            hash_table_insert(&sema->type_param_table, gdef->type_params[i].name,
                              (void *)resolved_args[i]);
        }

        ASTNode *orig = gdef->decl;
        FnSig *sig = rsg_malloc(sizeof(*sig));
        sig->name = mangled;
        sig->param_count = BUF_LEN(orig->fn_decl.params);
        sig->param_types = NULL;
        sig->param_names = NULL;
        sig->is_pub = false;
        for (int32_t i = 0; i < sig->param_count; i++) {
            ASTNode *param = orig->fn_decl.params[i];
            const Type *pt = resolve_ast_type(sema, &param->param.type);
            if (pt == NULL) {
                pt = &TYPE_ERR_INST;
            }
            BUF_PUSH(sig->param_types, pt);
            BUF_PUSH(sig->param_names, param->param.name);
        }
        const Type *ret = resolve_ast_type(sema, &orig->fn_decl.return_type);
        sig->return_type = (ret != NULL) ? ret : &TYPE_UNIT_INST;

        // Clear type param substitutions
        hash_table_destroy(&sema->type_param_table);
        hash_table_init(&sema->type_param_table, NULL);

        // Register concrete FnSig
        hash_table_insert(&sema->fn_table, mangled, sig);

        // Queue pending instantiation for deferred body checking
        GenericInst inst = {.generic = gdef,
                            .mangled_name = mangled,
                            .type_args = resolved_args,
                            .file_node = sema->file_node};
        BUF_PUSH(sema->pending_insts, inst);

        // Rewrite call to use mangled name
        node->call.callee->id.name = mangled;
        return resolve_call(sema, node, sig);
    }

    // Look up fn return type and check arg types
    if (fn_name != NULL) {
        FnSig *sig = sema_lookup_fn(sema, fn_name);
        if (sig != NULL) {
            return resolve_call(sema, node, sig);
        }

        // Try generic fn type inference: identity(42) → identity<i32>(42)
        GenericFnDef *gdef = sema_lookup_generic_fn(sema, fn_name);
        if (gdef != NULL) {
            ASTNode *orig = gdef->decl;
            int32_t num_params = gdef->type_param_count;
            int32_t arg_count = BUF_LEN(node->call.args);
            int32_t param_count = BUF_LEN(orig->fn_decl.params);

            if (arg_count == param_count) {
                const Type **inferred =
                    (const Type **)arena_alloc_zero(sema->arena, num_params * sizeof(const Type *));

                for (int32_t pi = 0; pi < param_count; pi++) {
                    ASTNode *param = orig->fn_decl.params[pi];
                    if (param->param.type.kind != AST_TYPE_NAME) {
                        continue;
                    }
                    for (int32_t ti = 0; ti < num_params; ti++) {
                        if (strcmp(param->param.type.name, gdef->type_params[ti].name) == 0) {
                            const Type *arg_type = node->call.args[pi]->type;
                            if (arg_type != NULL && arg_type->kind != TYPE_ERR) {
                                inferred[ti] = arg_type;
                            }
                            break;
                        }
                    }
                }

                bool all_inferred = true;
                for (int32_t i = 0; i < num_params; i++) {
                    if (inferred[i] == NULL) {
                        all_inferred = false;
                        break;
                    }
                }

                if (all_inferred) {
                    // Build synthetic type_args and set on node
                    ASTType *synth_args = NULL;
                    for (int32_t i = 0; i < num_params; i++) {
                        ASTType arg = {0};
                        arg.kind = AST_TYPE_NAME;
                        arg.name = type_name(sema->arena, inferred[i]);
                        BUF_PUSH(synth_args, arg);
                    }
                    node->call.type_args = synth_args;

                    // Recurse into generic call handling (type_args now present)
                    return check_call(sema, node);
                }
            }
        }

        Sym *sym = scope_lookup(sema, fn_name);
        if (sym != NULL && sym->kind == SYM_FN) {
            return sym->type;
        }
        // Call through fn-typed variable: f(args)
        if (sym != NULL && sym->type != NULL && sym->type->kind == TYPE_FN) {
            int32_t arg_count = BUF_LEN(node->call.args);
            int32_t fn_param_count = sym->type->fn_type.param_count;
            if (arg_count != fn_param_count) {
                SEMA_ERR(sema, node->loc, "expected %d args, got %d", fn_param_count, arg_count);
            } else {
                for (int32_t i = 0; i < arg_count; i++) {
                    const Type *param_type = sym->type->fn_type.params[i];
                    promote_lit(node->call.args[i], param_type);
                    const Type *arg_type = node->call.args[i]->type;
                    if (arg_type != NULL && param_type != NULL &&
                        !type_assignable(arg_type, param_type) && arg_type->kind != TYPE_ERR &&
                        param_type->kind != TYPE_ERR) {
                        SEMA_ERR(
                            sema, node->call.args[i]->loc, "type mismatch: expected '%s', got '%s'",
                            type_name(sema->arena, param_type), type_name(sema->arena, arg_type));
                    }
                }
            }
            return sym->type->fn_type.return_type;
        }
        if (sym == NULL) {
            // Handle bare Some(x) → Option<T> variant constructor
            if (strcmp(fn_name, "Some") == 0 && BUF_LEN(node->call.args) == 1) {
                const Type *arg_type = node->call.args[0]->type;
                if (arg_type != NULL && arg_type->kind != TYPE_ERR) {
                    GenericEnumDef *gdef = sema_lookup_generic_enum(sema, "Option");
                    if (gdef != NULL) {
                        ASTType val_arg = {.kind = AST_TYPE_NAME,
                                           .name = type_name(sema->arena, arg_type),
                                           .type_args = NULL,
                                           .loc = node->loc};
                        hash_table_insert(&sema->type_param_table, val_arg.name, (void *)arg_type);
                        GenericInstArgs inst_args = {&val_arg, 1, node->loc};
                        const char *mangled = instantiate_generic_enum(sema, gdef, &inst_args);
                        hash_table_remove(&sema->type_param_table, val_arg.name);
                        if (mangled != NULL) {
                            const Type *opt_type = sema_lookup_type_alias(sema, mangled);
                            if (opt_type != NULL) {
                                ASTNode *callee_member =
                                    ast_new(sema->arena, NODE_MEMBER, node->loc);
                                callee_member->member.object =
                                    ast_new(sema->arena, NODE_ID, node->loc);
                                callee_member->member.object->id.name = mangled;
                                callee_member->member.object->type = opt_type;
                                callee_member->member.member = "Some";
                                node->call.callee = callee_member;
                                return check_enum_variant_call(sema, node, opt_type, "Some");
                            }
                        }
                    }
                }
            }
            // Handle bare Ok(x) / Err(x) → Result<T, E> variant constructor
            if ((strcmp(fn_name, "Ok") == 0 || strcmp(fn_name, "Err") == 0) &&
                BUF_LEN(node->call.args) == 1) {
                const Type *expected = sema->expected_type;
                if (expected == NULL) {
                    expected = sema->fn_return_type;
                }
                if (expected != NULL && expected->kind == TYPE_ENUM) {
                    const Type *arg_type = node->call.args[0]->type;
                    if (arg_type != NULL && arg_type->kind != TYPE_ERR) {
                        const EnumVariant *variant = type_enum_find_variant(expected, fn_name);
                        if (variant != NULL) {
                            ASTNode *callee_member = ast_new(sema->arena, NODE_MEMBER, node->loc);
                            callee_member->member.object = ast_new(sema->arena, NODE_ID, node->loc);
                            callee_member->member.object->id.name = type_enum_name(expected);
                            callee_member->member.object->type = expected;
                            callee_member->member.member = fn_name;
                            node->call.callee = callee_member;
                            return check_enum_variant_call(sema, node, expected, fn_name);
                        }
                    }
                }
            }
            SEMA_ERR(sema, node->loc, "undefined function '%s'", fn_name);
        }
    }

    // General expression callee: idx, member field, etc.
    if (fn_name == NULL && node->call.callee->kind != NODE_CLOSURE) {
        const Type *ct = check_node(sema, node->call.callee);
        if (ct != NULL && ct->kind == TYPE_FN) {
            int32_t arg_count = BUF_LEN(node->call.args);
            int32_t fn_param_count = ct->fn_type.param_count;
            if (arg_count != fn_param_count) {
                SEMA_ERR(sema, node->loc, "expected %d args, got %d", fn_param_count, arg_count);
            } else {
                for (int32_t i = 0; i < arg_count; i++) {
                    const Type *param_type = ct->fn_type.params[i];
                    promote_lit(node->call.args[i], param_type);
                    const Type *arg_type = node->call.args[i]->type;
                    if (arg_type != NULL && param_type != NULL &&
                        !type_assignable(arg_type, param_type) && arg_type->kind != TYPE_ERR &&
                        param_type->kind != TYPE_ERR) {
                        SEMA_ERR(
                            sema, node->call.args[i]->loc, "type mismatch: expected '%s', got '%s'",
                            type_name(sema->arena, param_type), type_name(sema->arena, arg_type));
                    }
                }
            }
            return ct->fn_type.return_type;
        }
    }

    return &TYPE_ERR_INST;
}
