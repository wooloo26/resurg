#include "_check.h"

/**
 * @file check_call.c
 * @brief Call-expression type checking — fn calls, method dispatch, generics.
 *
 * Handles direct fn calls, member method calls, generic fn instantiation,
 * type inference, enum variant constructors, closure calls, and named args.
 */

// ── Addressability helpers ────────────────────────────────────────

/** Return true if @p node is an addressable lvalue (variable, member, index). */
static bool is_lvalue(const ASTNode *node) {
    if (node == NULL) {
        return false;
    }
    switch (node->kind) {
    case NODE_ID:
    case NODE_MEMBER:
    case NODE_IDX:
    case NODE_ADDRESS_OF:
        return true;
    default:
        return false;
    }
}

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

/** Validate a call through a fn-typed value (variable, field, or expression). */
static const Type *check_fn_type_call(Sema *sema, ASTNode *node, const Type *fn_type) {
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
                         type_name(sema->arena, param_type), type_name(sema->arena, arg_type));
            }
        }
    }
    return fn_type->fn_type.return_type;
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
    // Reject pointer receiver on rvalue (literal, call result, etc.)
    if (sig->is_ptr_recv && !is_lvalue(node->call.callee->member.object)) {
        SEMA_ERR(sema, node->loc, "cannot call pointer receiver on rvalue");
        return &TYPE_ERR_INST;
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

    // Module-qualified call: mod::fn(args) or mod::Enum::Variant(args)
    if (obj_type != NULL && obj_type->kind == TYPE_MODULE) {
        const char *mod_name = obj_type->module_type.name;
        const char *qualified;
        if (strlen(mod_name) == 0) {
            qualified = method_name;
        } else {
            qualified = arena_sprintf(sema->arena, "%s.%s", mod_name, method_name);
        }

        // Try as fn call: mod::fn(args)
        FnSig *sig = sema_lookup_fn(sema, qualified);
        if (sig != NULL) {
            // Visibility check: fn must be pub for external access
            if (!sig->is_pub) {
                SEMA_ERR(sema, node->loc, "'%s' is private in module '%s'", method_name, mod_name);
                return &TYPE_ERR_INST;
            }
            // Rewrite callee to a simple id with the qualified name
            node->call.callee->kind = NODE_ID;
            node->call.callee->id.name = qualified;
            for (int32_t i = 0; i < BUF_LEN(node->call.args); i++) {
                check_node(sema, node->call.args[i]);
            }
            return resolve_call(sema, node, sig);
        }

        // Try as struct lit or enum access: mod::Type
        StructDef *sdef = sema_lookup_struct(sema, qualified);
        if (sdef != NULL) {
            node->call.callee->member.object->type = sdef->type;
            // Return the struct type — the caller will handle struct lit or method dispatch
            *out_fn_name = method_name;
            return NULL;
        }

        EnumDef *edef = sema_lookup_enum(sema, qualified);
        if (edef != NULL) {
            // Replace the object with the qualified enum type for further dispatch
            node->call.callee->member.object->type = edef->type;
            node->call.callee->member.object->id.name = qualified;
            obj_type = edef->type;
            // Fall through to enum variant handling below
        }

        // Try as sub-module: mod::inner::...
        const Type *sub_mod = (const Type *)hash_table_lookup(&sema->type_alias_table, qualified);
        if (sub_mod == NULL) {
            // Check scope for the qualified name as a module
            Sym *mod_sym = scope_lookup(sema, qualified);
            if (mod_sym != NULL && mod_sym->type != NULL && mod_sym->type->kind == TYPE_MODULE) {
                obj_type = mod_sym->type;
                // Continue — this falls through to the rest of the dispatch
            }
        }
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
            // Reject pointer receiver on rvalue
            if (sig->is_ptr_recv && !is_lvalue(node->call.callee->member.object)) {
                SEMA_ERR(sema, node->loc, "cannot call pointer receiver on rvalue");
                return &TYPE_ERR_INST;
            }
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
            return check_fn_type_call(sema, node, sf->type);
        }
    }

    // Extension method call on primitive types (i32, str, bool, f64, etc.)
    if (obj_type != NULL) {
        const char *prim_name = type_name(sema->arena, obj_type);
        if (prim_name != NULL) {
            const char *method_key = arena_sprintf(sema->arena, "%s.%s", prim_name, method_name);
            FnSig *sig = sema_lookup_fn(sema, method_key);
            if (sig != NULL) {
                // Reject pointer receiver on rvalue (literal, call result, etc.)
                if (sig->is_ptr_recv && !is_lvalue(node->call.callee->member.object)) {
                    SEMA_ERR(sema, node->loc, "cannot call pointer receiver on rvalue");
                    return &TYPE_ERR_INST;
                }
                for (int32_t i = 0; i < BUF_LEN(node->call.args); i++) {
                    check_node(sema, node->call.args[i]);
                }
                return resolve_call(sema, node, sig);
            }
        }
    }

    *out_fn_name = method_name;
    return NULL;
}

// ── Inline closure call ───────────────────────────────────────────

/** Check an inline closure call: (|x| expr)(args). */
static const Type *check_inline_closure_call(Sema *sema, ASTNode *node) {
    for (int32_t i = 0; i < BUF_LEN(node->call.args); i++) {
        check_node(sema, node->call.args[i]);
    }
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

// ── Generic fn instantiation ──────────────────────────────────────

/** Instantiate a generic fn call with explicit type args: fn_name<T>(args). */
/** Resolve type args for a generic fn call. Returns resolved args buf (caller must BUF_FREE). */
static const Type **resolve_generic_type_args(Sema *sema, ASTNode *node, GenericFnDef *gdef) {
    int32_t got = BUF_LEN(node->call.type_args);
    const Type **resolved_args = NULL;
    for (int32_t i = 0; i < got; i++) {
        const Type *t = resolve_ast_type(sema, &node->call.type_args[i]);
        if (t == NULL) {
            t = &TYPE_ERR_INST;
        }
        BUF_PUSH(resolved_args, t);
    }

    // Push type param substitutions early for constraint resolution (e.g., I::Item)
    for (int32_t i = 0; i < gdef->type_param_count; i++) {
        hash_table_insert(&sema->generics.type_params, gdef->type_params[i].name,
                          (void *)resolved_args[i]);
    }
    return resolved_args;
}

/** Validate pact bounds, associated type constraints, and where clauses. Returns false on error. */
static bool validate_generic_bounds(Sema *sema, ASTNode *node, GenericFnDef *gdef,
                                    const Type **resolved_args) {
    int32_t expected = gdef->type_param_count;

    // Check bounds
    for (int32_t i = 0; i < expected; i++) {
        for (int32_t b = 0; b < BUF_LEN(gdef->type_params[i].bounds); b++) {
            const char *bound = gdef->type_params[i].bounds[b];
            if (!type_satisfies_bound(sema, resolved_args[i], bound)) {
                SEMA_ERR(sema, node->loc, "'%s' does not satisfy pact bound '%s'",
                         type_name(sema->arena, resolved_args[i]), bound);
                return false;
            }
        }
        // Check associated type constraints: T: Pact<Item = i32>
        for (int32_t c = 0; c < BUF_LEN(gdef->type_params[i].assoc_constraints); c++) {
            ASTAssocConstraint *ac = &gdef->type_params[i].assoc_constraints[c];
            if (resolved_args[i]->kind != TYPE_STRUCT) {
                continue;
            }
            StructDef *sdef = sema_lookup_struct(sema, resolved_args[i]->struct_type.name);
            if (sdef == NULL) {
                continue;
            }
            const Type *assoc_resolved = NULL;
            for (int32_t a = 0; a < BUF_LEN(sdef->assoc_types); a++) {
                if (strcmp(sdef->assoc_types[a].name, ac->assoc_name) == 0 &&
                    sdef->assoc_types[a].concrete_type != NULL) {
                    assoc_resolved = resolve_ast_type(sema, sdef->assoc_types[a].concrete_type);
                    break;
                }
            }
            if (assoc_resolved == NULL || assoc_resolved->kind == TYPE_ERR) {
                SEMA_ERR(sema, node->loc, "'%s' has no associated type '%s' for pact '%s'",
                         type_name(sema->arena, resolved_args[i]), ac->assoc_name, ac->pact_name);
                return false;
            }
            // Resolve expected type (may reference other type params)
            const Type *expected_type = resolve_ast_type(sema, ac->expected_type);
            if (expected_type != NULL && expected_type->kind != TYPE_ERR &&
                !type_equal(assoc_resolved, expected_type)) {
                SEMA_ERR(sema, node->loc, "associated type '%s' is '%s', expected '%s'",
                         ac->assoc_name, type_name(sema->arena, assoc_resolved),
                         type_name(sema->arena, expected_type));
                return false;
            }
        }
    }

    // Check where clause bounds
    ASTWhereClause *wcs = gdef->decl->fn_decl.where_clauses;
    for (int32_t w = 0; w < BUF_LEN(wcs); w++) {
        const Type *wc_type = NULL;
        for (int32_t i = 0; i < expected; i++) {
            if (strcmp(gdef->type_params[i].name, wcs[w].type_name) == 0) {
                wc_type = resolved_args[i];
                break;
            }
        }
        if (wc_type == NULL) {
            continue;
        }
        // Handle associated type projection: I::Item: Clone
        if (wcs[w].assoc_member != NULL) {
            if (wc_type->kind == TYPE_STRUCT) {
                StructDef *sdef = sema_lookup_struct(sema, wc_type->struct_type.name);
                if (sdef != NULL) {
                    const Type *assoc_resolved = NULL;
                    for (int32_t a = 0; a < BUF_LEN(sdef->assoc_types); a++) {
                        if (strcmp(sdef->assoc_types[a].name, wcs[w].assoc_member) == 0 &&
                            sdef->assoc_types[a].concrete_type != NULL) {
                            assoc_resolved =
                                resolve_ast_type(sema, sdef->assoc_types[a].concrete_type);
                            break;
                        }
                    }
                    if (assoc_resolved != NULL && assoc_resolved->kind != TYPE_ERR) {
                        for (int32_t b = 0; b < BUF_LEN(wcs[w].bounds); b++) {
                            if (!type_satisfies_bound(sema, assoc_resolved, wcs[w].bounds[b])) {
                                SEMA_ERR(sema, node->loc,
                                         "'%s::%s' = '%s' does not satisfy pact bound '%s'",
                                         wcs[w].type_name, wcs[w].assoc_member,
                                         type_name(sema->arena, assoc_resolved), wcs[w].bounds[b]);
                                return false;
                            }
                        }
                    }
                }
            }
            continue;
        }
        for (int32_t b = 0; b < BUF_LEN(wcs[w].bounds); b++) {
            if (!type_satisfies_bound(sema, wc_type, wcs[w].bounds[b])) {
                SEMA_ERR(sema, node->loc, "'%s' does not satisfy pact bound '%s'",
                         type_name(sema->arena, wc_type), wcs[w].bounds[b]);
                return false;
            }
        }
    }
    return true;
}

/** Build a concrete FnSig from a generic template and queue a deferred instantiation. */
static const Type *emit_generic_fn_inst(Sema *sema, ASTNode *node, GenericFnDef *gdef,
                                        const Type **resolved_args, const char *mangled) {
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

    sema_reset_type_params(sema);

    hash_table_insert(&sema->fn_table, mangled, sig);
    GenericInst inst = {.generic = gdef,
                        .mangled_name = mangled,
                        .type_args = resolved_args,
                        .file_node = sema->file_node};
    BUF_PUSH(sema->pending_insts, inst);

    node->call.callee->id.name = mangled;
    return resolve_call(sema, node, sig);
}

/** Instantiate a generic fn call with explicit type args: fn_name<T>(args). */
static const Type *check_generic_fn_call(Sema *sema, ASTNode *node, const char *fn_name) {
    GenericFnDef *gdef = sema_lookup_generic_fn(sema, fn_name);
    if (gdef == NULL) {
        SEMA_ERR(sema, node->loc, "undefined generic function '%s'", fn_name);
        return &TYPE_ERR_INST;
    }
    int32_t expected = gdef->type_param_count;
    int32_t got = BUF_LEN(node->call.type_args);
    if (got != expected) {
        SEMA_ERR(sema, node->loc, "wrong number of type arguments for '%s': expected %d, got %d",
                 fn_name, expected, got);
        return &TYPE_ERR_INST;
    }

    const Type **resolved_args = resolve_generic_type_args(sema, node, gdef);

    if (!validate_generic_bounds(sema, node, gdef, resolved_args)) {
        sema_reset_type_params(sema);
        return &TYPE_ERR_INST;
    }

    const char *mangled = build_mangled_name(sema, fn_name, resolved_args, got);
    FnSig *existing = sema_lookup_fn(sema, mangled);
    if (existing != NULL) {
        sema_reset_type_params(sema);
        node->call.callee->id.name = mangled;
        return resolve_call(sema, node, existing);
    }

    return emit_generic_fn_inst(sema, node, gdef, resolved_args, mangled);
}

// ── Generic type inference ────────────────────────────────────────

/** Try to infer type args from call args: identity(42) → identity<i32>(42). */
static const Type *infer_generic_call(Sema *sema, ASTNode *node, const char *fn_name) {
    GenericFnDef *gdef = sema_lookup_generic_fn(sema, fn_name);
    if (gdef == NULL) {
        return NULL;
    }
    ASTNode *orig = gdef->decl;
    int32_t num_params = gdef->type_param_count;
    int32_t arg_count = BUF_LEN(node->call.args);
    int32_t param_count = BUF_LEN(orig->fn_decl.params);
    if (arg_count != param_count) {
        return NULL;
    }

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

    for (int32_t i = 0; i < num_params; i++) {
        if (inferred[i] == NULL) {
            return NULL;
        }
    }

    // Build synthetic type_args and recurse
    ASTType *synth_args = NULL;
    for (int32_t i = 0; i < num_params; i++) {
        ASTType arg = {0};
        arg.kind = AST_TYPE_NAME;
        arg.name = type_name(sema->arena, inferred[i]);
        BUF_PUSH(synth_args, arg);
    }
    node->call.type_args = synth_args;
    return check_call(sema, node);
}

// ── Bare variant constructors ─────────────────────────────────────

/** Rewrite callee to a member access and check as enum variant call. */
static const Type *rewrite_variant_call(Sema *sema, ASTNode *node, const Type *enum_type,
                                        const char *variant_name) {
    ASTNode *callee_member = ast_new(sema->arena, NODE_MEMBER, node->loc);
    callee_member->member.object = ast_new(sema->arena, NODE_ID, node->loc);
    callee_member->member.object->id.name = type_enum_name(enum_type);
    callee_member->member.object->type = enum_type;
    callee_member->member.member = variant_name;
    node->call.callee = callee_member;
    return check_enum_variant_call(sema, node, enum_type, variant_name);
}

/** Handle bare Some(x) → Option<T> and bare Ok(x)/Err(x) → Result<T,E>. */
static const Type *check_bare_variant_call(Sema *sema, ASTNode *node, const char *fn_name) {
    if (BUF_LEN(node->call.args) != 1) {
        return NULL;
    }

    // bare Some(x) → Option<T>
    if (strcmp(fn_name, "Some") == 0) {
        const Type *arg_type = node->call.args[0]->type;
        if (arg_type == NULL || arg_type->kind == TYPE_ERR) {
            return NULL;
        }
        GenericEnumDef *gdef = sema_lookup_generic_enum(sema, "Option");
        if (gdef == NULL) {
            return NULL;
        }
        ASTType val_arg = {.kind = AST_TYPE_NAME,
                           .name = type_name(sema->arena, arg_type),
                           .type_args = NULL,
                           .loc = node->loc};
        hash_table_insert(&sema->generics.type_params, val_arg.name, (void *)arg_type);
        GenericInstArgs inst_args = {&val_arg, 1, node->loc};
        const char *mangled = instantiate_generic_enum(sema, gdef, &inst_args);
        hash_table_remove(&sema->generics.type_params, val_arg.name);
        if (mangled != NULL) {
            const Type *opt_type = sema_lookup_type_alias(sema, mangled);
            if (opt_type != NULL) {
                return rewrite_variant_call(sema, node, opt_type, "Some");
            }
        }
        return NULL;
    }

    // bare Ok(x) / Err(x) → Result<T, E>
    if (strcmp(fn_name, "Ok") == 0 || strcmp(fn_name, "Err") == 0) {
        const Type *expected = sema->expected_type;
        if (expected == NULL) {
            expected = sema->fn_return_type;
        }
        if (expected == NULL || expected->kind != TYPE_ENUM) {
            return NULL;
        }
        const Type *arg_type = node->call.args[0]->type;
        if (arg_type == NULL || arg_type->kind == TYPE_ERR) {
            return NULL;
        }
        const EnumVariant *variant = type_enum_find_variant(expected, fn_name);
        if (variant != NULL) {
            return rewrite_variant_call(sema, node, expected, fn_name);
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
        const Type *saved = sema->expected_type;
        if (early_sig != NULL && i < early_sig->param_count) {
            sema->expected_type = early_sig->param_types[i];
        } else if (callee_fn_type != NULL && i < callee_fn_type->fn_type.param_count) {
            sema->expected_type = callee_fn_type->fn_type.params[i];
        }
        check_node(sema, node->call.args[i]);
        sema->expected_type = saved;
    }
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
        return check_inline_closure_call(sema, node);
    }

    check_call_args(sema, node, fn_name);

    // Built-in fns
    if (fn_name != NULL && strcmp(fn_name, "assert") == 0) {
        return &TYPE_UNIT_INST;
    }
    if (fn_name != NULL && (strcmp(fn_name, "print") == 0 || strcmp(fn_name, "println") == 0)) {
        return &TYPE_UNIT_INST;
    }

    // Generic call: fn_name<Type, ...>(args)
    if (fn_name != NULL && BUF_LEN(node->call.type_args) > 0) {
        return check_generic_fn_call(sema, node, fn_name);
    }

    // Named fn lookup
    if (fn_name != NULL) {
        FnSig *sig = sema_lookup_fn(sema, fn_name);

        // Module-qualified fallback: try current module prefix
        if (sig == NULL && sema->current_scope->module_name != NULL) {
            const char *qualified =
                arena_sprintf(sema->arena, "%s.%s", sema->current_scope->module_name, fn_name);
            sig = sema_lookup_fn(sema, qualified);
            if (sig != NULL) {
                node->call.callee->id.name = qualified;
            }
        }

        if (sig != NULL) {
            return resolve_call(sema, node, sig);
        }

        // Try generic type inference
        const Type *inferred = infer_generic_call(sema, node, fn_name);
        if (inferred != NULL) {
            return inferred;
        }

        Sym *sym = scope_lookup(sema, fn_name);
        if (sym != NULL && sym->kind == SYM_FN) {
            return sym->type;
        }
        if (sym != NULL && sym->type != NULL && sym->type->kind == TYPE_FN) {
            return check_fn_type_call(sema, node, sym->type);
        }
        if (sym == NULL) {
            const Type *variant_result = check_bare_variant_call(sema, node, fn_name);
            if (variant_result != NULL) {
                return variant_result;
            }
            SEMA_ERR(sema, node->loc, "undefined function '%s'", fn_name);
        }
    }

    // General expression callee: idx, member field, etc.
    if (fn_name == NULL && node->call.callee->kind != NODE_CLOSURE) {
        const Type *ct = check_node(sema, node->call.callee);
        if (ct != NULL && ct->kind == TYPE_FN) {
            return check_fn_type_call(sema, node, ct);
        }
    }

    return &TYPE_ERR_INST;
}
