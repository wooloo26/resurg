#include "_check.h"

// ── Shared helpers ─────────────────────────────────────────────────────

ASTNode *build_none_variant_call(Arena *arena, const Type *enum_type, SrcLoc loc) {
    ASTNode *call = ast_new(arena, NODE_CALL, loc);
    ASTNode *callee = ast_new(arena, NODE_MEMBER, loc);
    callee->member.object = ast_new(arena, NODE_ID, loc);
    callee->member.object->id.name = type_enum_name(enum_type);
    callee->member.object->type = enum_type;
    callee->member.member = "None";
    call->call.callee = callee;
    call->call.args = NULL;
    call->call.arg_names = NULL;
    call->call.arg_is_mut = NULL;
    call->call.type_args = NULL;
    call->type = enum_type;
    return call;
}

const Type *find_promoted_field(Sema *sema, const StructDef *sdef, const char *field_name) {
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
    return NULL;
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
                    ASTNode *none_call = build_none_variant_call(sema->arena, ctx, node->loc);
                    node->kind = none_call->kind;
                    node->call = none_call->call;
                    node->type = ctx;
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
            FnTypeSpec fn_spec = {sig->param_types, sig->param_count, sig->return_type, FN_PLAIN};
            return type_create_fn(sema->arena, &fn_spec);
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

// ── Member access ──────────────────────────────────────────────────────

const Type *check_member(Sema *sema, ASTNode *node) {
    const Type *object_type = check_node(sema, node->member.object);

    // Auto-deref: if object is a ptr, unwrap to pointee
    if (object_type != NULL && object_type->kind == TYPE_PTR) {
        object_type = object_type->ptr.pointee;
    }

    // Module member access: mod::item
    if (object_type != NULL && object_type->kind == TYPE_MODULE) {
        const char *mod_name = object_type->module_type.name;
        const char *member = node->member.member;
        const char *qualified = arena_sprintf(sema->arena, "%s.%s", mod_name, member);

        // Try struct: mod::StructName
        StructDef *sdef = sema_lookup_struct(sema, qualified);
        if (sdef != NULL) {
            // Rewrite node to look like a direct struct ref
            node->kind = NODE_ID;
            node->id.name = qualified;
            return sdef->type;
        }

        // Try enum: mod::EnumName
        EnumDef *edef = sema_lookup_enum(sema, qualified);
        if (edef != NULL) {
            node->kind = NODE_ID;
            node->id.name = qualified;
            return edef->type;
        }

        // Try type alias: mod::TypeName
        const Type *talias = sema_lookup_type_alias(sema, qualified);
        if (talias != NULL) {
            return talias;
        }

        // Try sub-module: mod::inner
        Sym *sub = scope_lookup(sema, qualified);
        if (sub != NULL && sub->type != NULL && sub->type->kind == TYPE_MODULE) {
            return sub->type;
        }

        // Try fn (for fn-as-value): mod::fn_name
        FnSig *sig = sema_lookup_fn(sema, qualified);
        if (sig != NULL) {
            node->kind = NODE_ID;
            node->id.name = qualified;
            // Return fn type
            const Type **params = NULL;
            for (int32_t i = 0; i < sig->param_count; i++) {
                BUF_PUSH(params, sig->param_types[i]);
            }
            FnTypeSpec fn_spec = {params, sig->param_count, sig->return_type, FN_PLAIN};
            return type_create_fn(sema->arena, &fn_spec);
        }

        SEMA_ERR(sema, node->loc, "'%s' not found in module '%s'", member, mod_name);
        return &TYPE_ERR_INST;
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
            const Type *promoted = find_promoted_field(sema, sdef, field_name);
            if (promoted != NULL) {
                return promoted;
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

/**
 * Infer generic type args for a struct lit from its field values.
 * Returns the mangled struct name on success, or NULL if inference fails.
 */
static const char *infer_generic_struct_args(Sema *sema, ASTNode *node, GenericStructDef *gdef) {
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
        for (int32_t pi = 0; pi < num_params; pi++) {
            if (strcmp(ast_field->type.name, gdef->type_params[pi].name) == 0) {
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
    for (int32_t i = 0; i < num_params; i++) {
        if (inferred_args[i] == NULL) {
            return NULL;
        }
    }

    // Build synthetic type_args from inferred types
    ASTType *synth_args = NULL;
    for (int32_t i = 0; i < num_params; i++) {
        ASTType arg = {0};
        arg.kind = AST_TYPE_NAME;
        arg.name = type_name(sema->arena, inferred_args[i]);
        BUF_PUSH(synth_args, arg);
    }
    GenericInstArgs inst_args = {synth_args, num_params, node->loc};
    const char *mangled = instantiate_generic_struct(sema, gdef, &inst_args);
    BUF_FREE(synth_args);
    return mangled;
}

/** Resolve struct literal name through aliases, generics, and type inference. */
static StructDef *resolve_struct_lit(Sema *sema, ASTNode *node) {
    const char *struct_name = node->struct_lit.name;
    StructDef *sdef = sema_lookup_struct(sema, struct_name);

    // Follow type aliases: if no struct found, check if name is a type alias to a struct
    if (sdef == NULL && BUF_LEN(node->struct_lit.type_args) == 0) {
        const Type *alias = sema_lookup_type_alias(sema, struct_name);
        if (alias != NULL && alias->kind == TYPE_STRUCT) {
            sdef = sema_lookup_struct(sema, alias->struct_type.name);
            if (sdef != NULL) {
                node->struct_lit.name = sdef->name;
            }
        }
    }

    // If not found and has type args, try to instantiate from generic template
    if (sdef == NULL && BUF_LEN(node->struct_lit.type_args) > 0) {
        GenericStructDef *gdef = sema_lookup_generic_struct(sema, struct_name);
        if (gdef != NULL) {
            GenericInstArgs inst_args = {node->struct_lit.type_args,
                                         BUF_LEN(node->struct_lit.type_args), node->loc};
            const char *mangled = instantiate_generic_struct(sema, gdef, &inst_args);
            if (mangled != NULL) {
                node->struct_lit.name = mangled;
                sdef = sema_lookup_struct(sema, mangled);
            }
        }
    }

    // If still not found and has NO type args, try to infer from field values
    if (sdef == NULL && BUF_LEN(node->struct_lit.type_args) == 0) {
        GenericStructDef *gdef = sema_lookup_generic_struct(sema, struct_name);
        if (gdef != NULL) {
            const char *mangled = infer_generic_struct_args(sema, node, gdef);
            if (mangled != NULL) {
                node->struct_lit.name = mangled;
                sdef = sema_lookup_struct(sema, mangled);
            }
        }
    }

    return sdef;
}

const Type *check_struct_lit(Sema *sema, ASTNode *node) {
    const char *struct_name = node->struct_lit.name;
    StructDef *sdef = resolve_struct_lit(sema, node);
    if (sdef != NULL) {
        struct_name = node->struct_lit.name;
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

    FnTypeSpec fn_spec = {param_types, param_count, return_type, fn_kind};
    return type_create_fn(sema->arena, &fn_spec);
}
