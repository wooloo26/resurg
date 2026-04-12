#include "_check.h"

// ── Struct literal checking ────────────────────────────────────────────

/** Promote and type-check a field value against an expected type. */
void check_field_match(Sema *sema, ASTNode *value_node, const Type *expected_type) {
    promote_lit(value_node, expected_type);
    const Type *actual_type = value_node->type;
    if (actual_type != NULL && expected_type != NULL &&
        !type_assignable(actual_type, expected_type) && actual_type->kind != TYPE_ERR &&
        expected_type->kind != TYPE_ERR) {
        SEMA_ERR(sema, value_node->loc, "type mismatch: expected '%s', got '%s'",
                 type_name(sema->base.arena, expected_type),
                 type_name(sema->base.arena, actual_type));
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
        (const Type **)arena_alloc_zero(sema->base.arena, num_params * sizeof(const Type *));

    // Check field values first to get their types
    for (int32_t i = 0; i < BUF_LEN(node->struct_lit.field_values); i++) {
        check_node(sema, node->struct_lit.field_values[i]);
    }

    // Match field types against generic params
    for (int32_t fi = 0; fi < BUF_LEN(gdef->decl->struct_decl->fields); fi++) {
        ASTStructField *ast_field = &gdef->decl->struct_decl->fields[fi];
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
        arg.name = type_name(sema->base.arena, inferred_args[i]);
        BUF_PUSH(synth_args, arg);
    }
    GenericInstArgs inst_args = {synth_args, num_params, node->loc};
    const char *mangled = instantiate_generic_struct(sema, gdef, &inst_args);
    BUF_FREE(synth_args);
    return mangled;
}

/**
 * Try to resolve a generic type alias by pushing @p got explicit type args
 * (filling remaining from defaults).  Returns the resolved StructDef or NULL.
 */
static StructDef *try_resolve_generic_type_alias(Sema *sema, ASTNode *node, GenericTypeAlias *gta,
                                                 ASTType *type_args, int32_t got) {
    int32_t expected = gta->base.type_param_count;
    int32_t min_required = 0;
    for (int32_t i = 0; i < expected; i++) {
        if (gta->base.type_params[i].default_type == NULL) {
            min_required = i + 1;
        }
    }
    if (got < min_required || got > expected) {
        return NULL;
    }
    // Push explicit type params
    for (int32_t i = 0; i < got; i++) {
        const Type *t = resolve_ast_type(sema, &type_args[i]);
        if (t == NULL) {
            t = &TYPE_ERR_INST;
        }
        hash_table_insert(&sema->base.generics.type_params, gta->base.type_params[i].name,
                          (void *)t);
    }
    // Fill defaults for remaining
    for (int32_t i = got; i < expected; i++) {
        const Type *t = resolve_ast_type(sema, gta->base.type_params[i].default_type);
        if (t == NULL) {
            t = &TYPE_ERR_INST;
        }
        hash_table_insert(&sema->base.generics.type_params, gta->base.type_params[i].name,
                          (void *)t);
    }
    const Type *result = resolve_ast_type(sema, &gta->alias_type);
    for (int32_t i = 0; i < expected; i++) {
        hash_table_remove(&sema->base.generics.type_params, gta->base.type_params[i].name);
    }
    if (result != NULL && result->kind == TYPE_STRUCT) {
        StructDef *sdef = sema_lookup_struct(sema, result->struct_type.name);
        if (sdef != NULL) {
            node->struct_lit.name = sdef->name;
        }
        return sdef;
    }
    return NULL;
}

static StructDef *resolve_struct_lit(Sema *sema, ASTNode *node) {
    const char *struct_name = node->struct_lit.name;

    // Resolve Self to the enclosing type name
    if (strcmp(struct_name, "Self") == 0 && sema->infer.self_type_name != NULL) {
        struct_name = sema->infer.self_type_name;
        node->struct_lit.name = struct_name;
    }

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

    // If not found and has type args, try generic type alias
    if (sdef == NULL && BUF_LEN(node->struct_lit.type_args) > 0) {
        GenericTypeAlias *gta = sema_lookup_generic_type_alias(sema, struct_name);
        if (gta != NULL) {
            sdef = try_resolve_generic_type_alias(sema, node, gta, node->struct_lit.type_args,
                                                  BUF_LEN(node->struct_lit.type_args));
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

    // If still not found, check generic type aliases where all params have defaults
    if (sdef == NULL && BUF_LEN(node->struct_lit.type_args) == 0) {
        GenericTypeAlias *gta = sema_lookup_generic_type_alias(sema, struct_name);
        if (gta != NULL) {
            sdef = try_resolve_generic_type_alias(sema, node, gta, NULL, 0);
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
    bool foreign = is_foreign_struct(sema, sdef->name);
    int32_t provided_count = BUF_LEN(node->struct_lit.field_names);
    for (int32_t i = 0; i < provided_count; i++) {
        const char *fname = node->struct_lit.field_names[i];
        ASTNode *fvalue = node->struct_lit.field_values[i];

        // Find the field type for expected_type propagation
        const Type *field_type = NULL;
        bool field_is_pub = false;
        for (int32_t j = 0; j < BUF_LEN(sdef->fields); j++) {
            if (strcmp(sdef->fields[j].name, fname) == 0) {
                field_type = sdef->fields[j].type;
                field_is_pub = sdef->fields[j].is_pub;
                break;
            }
        }

        // Check field visibility for cross-module access
        if (field_type != NULL && !field_is_pub && foreign) {
            SEMA_ERR(sema, fvalue->loc, "field '%s' is private in struct '%s'", fname, struct_name);
        }

        // Set expected type before checking (aids closure param inference)
        SEMA_INFER_SCOPE(sema, expected_type,
                         field_type != NULL ? field_type : sema->infer.expected_type);
        check_node(sema, fvalue);
        SEMA_INFER_RESTORE(sema, expected_type);

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
