#include "_check.h"

// ── Forward declarations ──────────────────────────────────────────

// ── Named-arg helpers ─────────────────────────────────────────────

/**
 * Reorder call args to match param poss using named labels.
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
            int32_t idx = -1;
            for (int32_t j = 0; j < sig->param_count; j++) {
                if (strcmp(sig->param_names[j], aname) == 0) {
                    idx = j;
                    break;
                }
            }
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
 * Validate arg count against @p sig and type-check each arg.
 * Promotes lit args to match the corresponding param type.
 */
static void check_call_args(Sema *sema, ASTNode *node, const FnSig *sig) {
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

// ── Operator classification ────────────────────────────────────────────

static bool binary_op_yields_bool(TokenKind op) {
    return (op >= TOKEN_EQUAL_EQUAL && op <= TOKEN_GREATER_EQUAL) ||
           op == TOKEN_AMPERSAND_AMPERSAND || op == TOKEN_PIPE_PIPE;
}

// ── Expression checkers ────────────────────────────────────────────────

const Type *check_lit(Sema *sema, ASTNode *node) {
    (void)sema;
    return lit_kind_to_type(node->lit.kind);
}

const Type *check_id(Sema *sema, ASTNode *node) {
    Sym *sym = scope_lookup(sema, node->id.name);
    if (sym == NULL) {
        // Handle bare `None` — resolve from expected type or fn return type
        if (strcmp(node->id.name, "None") == 0) {
            const Type *ctx = sema->expected_type;
            if (ctx == NULL || ctx->kind != TYPE_ENUM) {
                ctx = sema->fn_return_type;
            }
            if (ctx != NULL && ctx->kind == TYPE_ENUM) {
                const EnumVariant *variant = type_enum_find_variant(ctx, "None");
                if (variant != NULL) {
                    // Rewrite to enum variant call for lowering
                    node->kind = NODE_CALL;
                    ASTNode *callee_member = ast_new(sema->arena, NODE_MEMBER, node->loc);
                    callee_member->member.object = ast_new(sema->arena, NODE_ID, node->loc);
                    callee_member->member.object->id.name = type_enum_name(ctx);
                    callee_member->member.object->type = ctx;
                    callee_member->member.member = "None";
                    node->call.callee = callee_member;
                    node->call.args = NULL;
                    node->call.arg_names = NULL;
                    node->call.arg_is_mut = NULL;
                    node->call.type_args = NULL;
                    return ctx;
                }
            }
        }
        SEMA_ERR(sema, node->loc, "undefined variable '%s'", node->id.name);
        return &TYPE_ERR_INST;
    }
    // Track captured variable references for Fn/FnMut auto-inference
    if (sema->closure_scope != NULL && sym->kind != SYM_FN && sym->kind != SYM_TYPE) {
        bool found_local = false;
        for (Scope *s = sema->current_scope; s != NULL && s != sema->closure_scope->parent;
             s = s->parent) {
            if (hash_table_lookup(&s->table, node->id.name) != NULL) {
                found_local = true;
                break;
            }
        }
        if (!found_local) {
            sema->closure_has_capture = true;
        }
    }
    // Function symbols used as values → construct fn type
    if (sym->kind == SYM_FN) {
        FnSig *sig = sema_lookup_fn(sema, node->id.name);
        if (sig != NULL) {
            return type_create_fn(sema->arena, sig->param_types, sig->param_count, sig->return_type,
                                  FN_PLAIN);
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
                     type_name(sema->arena, operand));
            return &TYPE_ERR_INST;
        }
        return &TYPE_BOOL_INST;
    }
    if (node->unary.op == TOKEN_MINUS) {
        if (!type_is_numeric(operand)) {
            SEMA_ERR(sema, node->loc, "'-' requires numeric operand, got '%s'",
                     type_name(sema->arena, operand));
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
        return binary_op_yields_bool(node->binary.op) ? &TYPE_BOOL_INST : &TYPE_ERR_INST;
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
            SEMA_ERR(sema, node->loc, "type mismatch: '%s' and '%s'", type_name(sema->arena, left),
                     type_name(sema->arena, right));
        }
    }

    // Boolean operations require bool operands
    if (node->binary.op == TOKEN_AMPERSAND_AMPERSAND || node->binary.op == TOKEN_PIPE_PIPE) {
        return &TYPE_BOOL_INST;
    }

    // Comparison operators return bool
    switch (node->binary.op) {
    case TOKEN_EQUAL_EQUAL:
    case TOKEN_BANG_EQUAL:
    case TOKEN_LESS:
    case TOKEN_LESS_EQUAL:
    case TOKEN_GREATER:
    case TOKEN_GREATER_EQUAL:
        return &TYPE_BOOL_INST;
    default:
        break;
    }

    // Arithmetic returns the operand type
    return left;
}

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

/** Reorder named args, validate, and apply a resolved fn sig to a call. */
static const Type *resolve_call(Sema *sema, ASTNode *node, const FnSig *sig) {
    reorder_named_args(sema, node, sig);
    check_call_args(sema, node, sig);
    node->type = sig->return_type;
    return sig->return_type;
}

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
            const char *mangled = instantiate_generic_enum(
                sema, gdef, node->call.type_args, BUF_LEN(node->call.type_args), node->loc);
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
        const Type *expected_fn =
            type_create_fn(sema->arena, param_types, param_count, NULL, FN_PLAIN);
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
                        const char *mangled =
                            instantiate_generic_enum(sema, gdef, &val_arg, 1, node->loc);
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

const Type *check_member(Sema *sema, ASTNode *node) {
    const Type *object_type = check_node(sema, node->member.object);

    // Auto-deref: if object is a ptr, unwrap to pointee
    if (object_type != NULL && object_type->kind == TYPE_PTR) {
        object_type = object_type->ptr.pointee;
    }

    // Slice .len access
    if (object_type != NULL && object_type->kind == TYPE_SLICE) {
        if (strcmp(node->member.member, "len") == 0) {
            return &TYPE_I32_INST;
        }
    }

    // Tuple field access: .0, .1, .2, ...
    if (object_type != NULL && object_type->kind == TYPE_TUPLE) {
        char *end = NULL;
        long idx = strtol(node->member.member, &end, 10);
        if (end != NULL && *end == '\0' && idx >= 0 && idx < object_type->tuple.count) {
            return object_type->tuple.elems[idx];
        }
    }
    // Struct field access
    if (object_type != NULL && object_type->kind == TYPE_STRUCT) {
        const char *field_name = node->member.member;

        // Check own fields (including embedded struct fields by name, e.g., e.Base)
        const StructField *sf = type_struct_find_field(object_type, field_name);
        if (sf != NULL) {
            return sf->type;
        }

        // Check promoted fields from embedded structs
        StructDef *sdef = sema_lookup_struct(sema, object_type->struct_type.name);
        if (sdef != NULL) {
            for (int32_t ei = 0; ei < BUF_LEN(sdef->embedded); ei++) {
                StructDef *embed_def = sema_lookup_struct(sema, sdef->embedded[ei]);
                if (embed_def != NULL) {
                    for (int32_t fi = 0; fi < BUF_LEN(embed_def->fields); fi++) {
                        if (strcmp(embed_def->fields[fi].name, field_name) == 0) {
                            return embed_def->fields[fi].type;
                        }
                    }
                }
            }
        }

        SEMA_ERR(sema, node->loc, "no field '%s' on type '%s'", field_name,
                 object_type->struct_type.name);
        return &TYPE_ERR_INST;
    }

    // Enum variant access: EnumType.Variant
    if (object_type != NULL && object_type->kind == TYPE_ENUM) {
        const EnumVariant *variant = type_enum_find_variant(object_type, node->member.member);
        if (variant != NULL) {
            return object_type;
        }
        SEMA_ERR(sema, node->loc, "no variant '%s' on enum '%s'", node->member.member,
                 type_enum_name(object_type));
        return &TYPE_ERR_INST;
    }

    return &TYPE_ERR_INST;
}

const Type *check_idx(Sema *sema, ASTNode *node) {
    const Type *object_type = check_node(sema, node->idx_access.object);
    check_node(sema, node->idx_access.idx);
    if (object_type != NULL && object_type->kind == TYPE_ARRAY) {
        return object_type->array.elem;
    }
    if (object_type != NULL && object_type->kind == TYPE_SLICE) {
        return object_type->slice.elem;
    }
    return &TYPE_ERR_INST;
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

/** Infer and unify elem types across all elems in a collection lit. */
static const Type *check_elem_list(Sema *sema, ASTNode **elems, ASTType *ast_elem_type) {
    const Type *elem_type = resolve_ast_type(sema, ast_elem_type);
    for (int32_t i = 0; i < BUF_LEN(elems); i++) {
        const Type *elem = check_node(sema, elems[i]);
        if (elem_type == NULL && elem != NULL) {
            elem_type = elem;
        }
        if (elem_type != NULL) {
            promote_lit(elems[i], elem_type);
        }
    }
    if (elem_type == NULL) {
        elem_type = &TYPE_I32_INST;
    }
    return elem_type;
}

const Type *check_array_lit(Sema *sema, ASTNode *node) {
    const Type *elem_type =
        check_elem_list(sema, node->array_lit.elems, &node->array_lit.elem_type);
    int32_t size = node->array_lit.size;
    if (size == 0) {
        size = BUF_LEN(node->array_lit.elems);
    }
    return type_create_array(sema->arena, elem_type, size);
}

const Type *check_slice_lit(Sema *sema, ASTNode *node) {
    const Type *elem_type =
        check_elem_list(sema, node->slice_lit.elems, &node->slice_lit.elem_type);
    return type_create_slice(sema->arena, elem_type);
}

const Type *check_slice_expr(Sema *sema, ASTNode *node) {
    const Type *object_type = check_node(sema, node->slice_expr.object);
    if (node->slice_expr.start != NULL) {
        check_node(sema, node->slice_expr.start);
    }
    if (node->slice_expr.end != NULL) {
        check_node(sema, node->slice_expr.end);
    }
    if (object_type != NULL && object_type->kind == TYPE_ARRAY) {
        return type_create_slice(sema->arena, object_type->array.elem);
    }
    if (object_type != NULL && object_type->kind == TYPE_SLICE) {
        return type_create_slice(sema->arena, object_type->slice.elem);
    }
    return &TYPE_ERR_INST;
}

const Type *check_tuple_lit(Sema *sema, ASTNode *node) {
    const Type ** /* buf */ elem_types = NULL;
    for (int32_t i = 0; i < BUF_LEN(node->tuple_lit.elems); i++) {
        const Type *elem = check_node(sema, node->tuple_lit.elems[i]);
        BUF_PUSH(elem_types, elem);
    }
    const Type *result = type_create_tuple(sema->arena, elem_types, BUF_LEN(elem_types));
    return result;
}

/** Promote and type-check a field value against an expected type. */
void check_field_match(Sema *sema, ASTNode *value_node, const Type *expected_type) {
    promote_lit(value_node, expected_type);
    const Type *actual_type = value_node->type;
    if (actual_type != NULL && expected_type != NULL &&
        !type_assignable(actual_type, expected_type) && actual_type->kind != TYPE_ERR &&
        expected_type->kind != TYPE_ERR) {
        SEMA_ERR(sema, value_node->loc, "type mismatch: expected '%s', got '%s'",
                 type_name(sema->arena, expected_type), type_name(sema->arena, actual_type));
    }
}

/** Check if struct lit already provides a field by name. */
static bool struct_lit_has_field(const ASTNode *node, const char *name) {
    for (int32_t i = 0; i < BUF_LEN(node->struct_lit.field_names); i++) {
        if (strcmp(node->struct_lit.field_names[i], name) == 0) {
            return true;
        }
    }
    return false;
}

const Type *check_struct_lit(Sema *sema, ASTNode *node) {
    const char *struct_name = node->struct_lit.name;
    StructDef *sdef = sema_lookup_struct(sema, struct_name);

    // Follow type aliases: if no struct found, check if name is a type alias to a struct
    if (sdef == NULL && BUF_LEN(node->struct_lit.type_args) == 0) {
        const Type *alias = sema_lookup_type_alias(sema, struct_name);
        if (alias != NULL && alias->kind == TYPE_STRUCT) {
            sdef = sema_lookup_struct(sema, alias->struct_type.name);
            if (sdef != NULL) {
                node->struct_lit.name = sdef->name;
                struct_name = sdef->name;
            }
        }
    }

    // If not found and has type args, try to instantiate from generic template
    if (sdef == NULL && BUF_LEN(node->struct_lit.type_args) > 0) {
        GenericStructDef *gdef = sema_lookup_generic_struct(sema, struct_name);
        if (gdef != NULL) {
            const char *mangled =
                instantiate_generic_struct(sema, gdef, node->struct_lit.type_args,
                                           BUF_LEN(node->struct_lit.type_args), node->loc);
            if (mangled != NULL) {
                node->struct_lit.name = mangled;
                struct_name = mangled;
                sdef = sema_lookup_struct(sema, mangled);
            }
        }
    }

    // If still not found and has NO type args, try to infer from field values
    if (sdef == NULL && BUF_LEN(node->struct_lit.type_args) == 0) {
        GenericStructDef *gdef = sema_lookup_generic_struct(sema, struct_name);
        if (gdef != NULL) {
            // Infer type args from field values
            int32_t num_params = gdef->type_param_count;
            const Type **inferred_args =
                (const Type **)arena_alloc_zero(sema->arena, num_params * sizeof(const Type *));

            // Check field values first to get their types
            for (int32_t i = 0; i < BUF_LEN(node->struct_lit.field_values); i++) {
                check_node(sema, node->struct_lit.field_values[i]);
            }

            // Match field types against generic params
            for (int32_t fi = 0; fi < BUF_LEN(gdef->decl->struct_decl.fields); fi++) {
                ASTStructField *ast_field = &gdef->decl->struct_decl.fields[fi];
                if (ast_field->type.kind != AST_TYPE_NAME) {
                    continue;
                }
                // Check if this field's type is a type parameter
                for (int32_t pi = 0; pi < num_params; pi++) {
                    if (strcmp(ast_field->type.name, gdef->type_params[pi].name) == 0) {
                        // Find the corresponding value
                        for (int32_t vi = 0; vi < BUF_LEN(node->struct_lit.field_names); vi++) {
                            if (strcmp(node->struct_lit.field_names[vi], ast_field->name) == 0) {
                                const Type *val_type = node->struct_lit.field_values[vi]->type;
                                if (val_type != NULL && val_type->kind != TYPE_ERR) {
                                    inferred_args[pi] = val_type;
                                }
                                break;
                            }
                        }
                        break;
                    }
                }
            }

            // Verify all params inferred
            bool all_inferred = true;
            for (int32_t i = 0; i < num_params; i++) {
                if (inferred_args[i] == NULL) {
                    all_inferred = false;
                    break;
                }
            }

            if (all_inferred) {
                // Build type_args from inferred types
                ASTType *synth_args = NULL;
                for (int32_t i = 0; i < num_params; i++) {
                    ASTType arg = {0};
                    arg.kind = AST_TYPE_NAME;
                    arg.name = type_name(sema->arena, inferred_args[i]);
                    BUF_PUSH(synth_args, arg);
                }
                const char *mangled =
                    instantiate_generic_struct(sema, gdef, synth_args, num_params, node->loc);
                if (mangled != NULL) {
                    node->struct_lit.name = mangled;
                    struct_name = mangled;
                    sdef = sema_lookup_struct(sema, mangled);
                }
                BUF_FREE(synth_args);
            }
        }
    }

    if (sdef == NULL) {
        SEMA_ERR(sema, node->loc, "unknown struct '%s'", struct_name);
        return &TYPE_ERR_INST;
    }

    // Check that all provided fields exist and have the right types
    int32_t provided_count = BUF_LEN(node->struct_lit.field_names);
    for (int32_t i = 0; i < provided_count; i++) {
        const char *fname = node->struct_lit.field_names[i];
        ASTNode *fvalue = node->struct_lit.field_values[i];

        // Find the field type for expected_type propagation
        const Type *field_type = NULL;
        for (int32_t j = 0; j < BUF_LEN(sdef->fields); j++) {
            if (strcmp(sdef->fields[j].name, fname) == 0) {
                field_type = sdef->fields[j].type;
                break;
            }
        }

        // Set expected type before checking (aids closure param inference)
        const Type *saved_expected = sema->expected_type;
        if (field_type != NULL) {
            sema->expected_type = field_type;
        }
        check_node(sema, fvalue);
        sema->expected_type = saved_expected;

        // Validate the field type
        if (field_type != NULL) {
            check_field_match(sema, fvalue, field_type);
        } else {
            SEMA_ERR(sema, node->loc, "no field '%s' on struct '%s'", fname, struct_name);
        }
    }

    // Check that all required fields (no default) are provided
    for (int32_t i = 0; i < BUF_LEN(sdef->fields); i++) {
        if (sdef->fields[i].default_value == NULL) {
            if (!struct_lit_has_field(node, sdef->fields[i].name)) {
                SEMA_ERR(sema, node->loc, "missing field '%s' in struct '%s'", sdef->fields[i].name,
                         struct_name);
            }
        }
    }

    // Fill in default values for unprovided fields
    for (int32_t i = 0; i < BUF_LEN(sdef->fields); i++) {
        if (sdef->fields[i].default_value != NULL) {
            if (!struct_lit_has_field(node, sdef->fields[i].name)) {
                BUF_PUSH(node->struct_lit.field_names, sdef->fields[i].name);
                BUF_PUSH(node->struct_lit.field_values, sdef->fields[i].default_value);
            }
        }
    }

    return sdef->type;
}

const Type *check_address_of(Sema *sema, ASTNode *node) {
    const Type *inner_type = check_node(sema, node->address_of.operand);
    if (inner_type == NULL || inner_type->kind == TYPE_ERR) {
        return &TYPE_ERR_INST;
    }
    // Addressability: only vars and struct lits are addressable
    ASTNode *operand = node->address_of.operand;
    if (operand->kind != NODE_ID && operand->kind != NODE_STRUCT_LIT &&
        operand->kind != NODE_MEMBER && operand->kind != NODE_IDX) {
        SEMA_ERR(sema, node->loc, "cannot take address of rvalue");
        return &TYPE_ERR_INST;
    }
    return type_create_ptr(sema->arena, inner_type, false);
}

const Type *check_deref(Sema *sema, ASTNode *node) {
    const Type *inner_type = check_node(sema, node->deref.operand);
    if (inner_type == NULL || inner_type->kind == TYPE_ERR) {
        return &TYPE_ERR_INST;
    }
    if (inner_type->kind != TYPE_PTR) {
        SEMA_ERR(sema, node->loc, "cannot deref non-ptr type '%s'",
                 type_name(sema->arena, inner_type));
        return &TYPE_ERR_INST;
    }
    return inner_type->ptr.pointee;
}

const Type *check_closure(Sema *sema, ASTNode *node) {
    const Type *expected = sema->expected_type;
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
    Scope *saved_closure_scope = sema->closure_scope;
    FnTypeKind saved_closure_fn_kind = sema->closure_fn_kind;
    bool saved_has_capture = sema->closure_has_capture;
    bool saved_captures_mutated = sema->closure_captures_mutated;

    // Always set up closure scope so capture tracking works
    sema->closure_scope = sema->current_scope;
    sema->closure_fn_kind = infer_fn_kind ? FN_CLOSURE_MUT : fn_kind;
    if (infer_fn_kind) {
        sema->closure_has_capture = false;
        sema->closure_captures_mutated = false;
    }

    const Type *body_type = check_node(sema, node->closure.body);

    // Auto-infer fn_kind from capture analysis
    if (infer_fn_kind) {
        if (sema->closure_captures_mutated) {
            fn_kind = FN_CLOSURE_MUT;
        } else if (sema->closure_has_capture) {
            fn_kind = FN_CLOSURE;
        }
    }

    sema->closure_scope = saved_closure_scope;
    sema->closure_fn_kind = saved_closure_fn_kind;
    sema->closure_has_capture = saved_has_capture;
    sema->closure_captures_mutated = saved_captures_mutated;
    scope_pop(sema);

    const Type *return_type = resolve_ast_type(sema, &node->closure.return_type);
    if (return_type == NULL) {
        return_type = (body_type != NULL) ? body_type : &TYPE_UNIT_INST;
    }

    return type_create_fn(sema->arena, param_types, param_count, return_type, fn_kind);
}
