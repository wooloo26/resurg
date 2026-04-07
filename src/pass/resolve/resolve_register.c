#include "_sema.h"

/**
 * @file resolve_register.c
 * @brief Declaration registration — first-pass symbol table population.
 *
 * Registers pacts, structs, enums, type aliases, and fn sigs into
 * the Sema tables so that subsequent passes can look them up.
 */

// ── Static helpers ─────────────────────────────────────────────────

/**
 * Collect fields for a struct: promoted fields from embedded structs first,
 * then the struct's own fields (with duplicate checking).
 */
static void compose_struct_fields(Sema *sema, const ASTNode *decl, StructDef *def) {
    // Promote fields from embedded structs
    for (int32_t i = 0; i < BUF_LEN(def->embedded); i++) {
        StructDef *embed_def = sema_lookup_struct(sema, def->embedded[i]);
        if (embed_def == NULL) {
            SEMA_ERR(sema, decl->loc, "unknown embedded struct '%s'", def->embedded[i]);
            continue;
        }
        for (int32_t j = 0; j < embed_def->type->struct_type.field_count; j++) {
            const StructField *ef = &embed_def->type->struct_type.fields[j];
            StructFieldInfo fi = {.name = ef->name, .type = ef->type, .default_value = NULL};
            for (int32_t k = 0; k < BUF_LEN(embed_def->fields); k++) {
                if (strcmp(embed_def->fields[k].name, ef->name) == 0) {
                    fi.default_value = embed_def->fields[k].default_value;
                    break;
                }
            }
            BUF_PUSH(def->fields, fi);
        }
    }

    // Add own fields, checking for duplicates against promoted fields
    for (int32_t i = 0; i < BUF_LEN(decl->struct_decl.fields); i++) {
        ASTStructField *ast_field = &decl->struct_decl.fields[i];
        bool duplicate = false;
        for (int32_t j = 0; j < BUF_LEN(def->fields); j++) {
            if (strcmp(def->fields[j].name, ast_field->name) == 0) {
                SEMA_ERR(sema, decl->loc, "duplicate field '%s'", ast_field->name);
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            const Type *field_type = resolve_ast_type(sema, &ast_field->type);
            if (field_type == NULL) {
                field_type = &TYPE_ERR_INST;
            }
            StructFieldInfo fi = {.name = ast_field->name,
                                  .type = field_type,
                                  .default_value = ast_field->default_value};
            BUF_PUSH(def->fields, fi);
        }
    }
}

/**
 * Build the Type* for a struct from its collected fields and embedded types,
 * and assign it to both the def and the AST decl node.
 */
static void build_struct_type(Sema *sema, ASTNode *decl, StructDef *def) {
    const Type **embedded_types = NULL;
    StructField *type_fields = NULL;

    // Add embedded struct fields as named fields (e.g., "Base")
    for (int32_t i = 0; i < BUF_LEN(def->embedded); i++) {
        StructDef *embed_def = sema_lookup_struct(sema, def->embedded[i]);
        if (embed_def != NULL) {
            BUF_PUSH(embedded_types, embed_def->type);
            StructField sf = {.name = def->embedded[i], .type = embed_def->type};
            BUF_PUSH(type_fields, sf);
        }
    }

    // Add own fields
    for (int32_t i = 0; i < BUF_LEN(decl->struct_decl.fields); i++) {
        ASTStructField *ast_field = &decl->struct_decl.fields[i];
        const Type *field_type = resolve_ast_type(sema, &ast_field->type);
        if (field_type == NULL) {
            field_type = &TYPE_ERR_INST;
        }
        StructField sf = {.name = ast_field->name, .type = field_type};
        BUF_PUSH(type_fields, sf);
    }

    StructTypeSpec struct_spec = {
        .name = def->name,
        .fields = type_fields,
        .field_count = BUF_LEN(type_fields),
        .embedded = embedded_types,
        .embed_count = BUF_LEN(embedded_types),
    };
    def->type = type_create_struct(sema->arena, &struct_spec);
    decl->type = def->type;
}

/**
 * Register a method's sig in the fn table (keyed as "TypeName.method_name")
 * and append a StructMethodInfo entry to the methods buf.
 */
static void register_method_sig(Sema *sema, const char *type_name, ASTNode *method,
                                StructMethodInfo **methods) {
    StructMethodInfo mi = {.name = method->fn_decl.name,
                           .is_mut_recv = method->fn_decl.is_mut_recv,
                           .is_ptr_recv = method->fn_decl.is_ptr_recv,
                           .recv_name = method->fn_decl.recv_name,
                           .decl = method};
    BUF_PUSH(*methods, mi);

    const char *method_key = arena_sprintf(sema->arena, "%s.%s", type_name, mi.name);

    const Type *return_type = &TYPE_UNIT_INST;
    if (method->fn_decl.return_type.kind != AST_TYPE_INFERRED) {
        return_type = resolve_ast_type(sema, &method->fn_decl.return_type);
        if (return_type == NULL) {
            return_type = &TYPE_UNIT_INST;
        }
    }

    FnSig *sig = rsg_malloc(sizeof(*sig));
    sig->name = mi.name;
    sig->return_type = return_type;
    sig->param_types = NULL;
    sig->param_names = NULL;
    sig->param_count = BUF_LEN(method->fn_decl.params);
    sig->is_pub = false;

    for (int32_t j = 0; j < sig->param_count; j++) {
        ASTNode *param = method->fn_decl.params[j];
        const Type *pt = resolve_ast_type(sema, &param->param.type);
        if (pt == NULL) {
            pt = &TYPE_ERR_INST;
        }
        BUF_PUSH(sig->param_types, pt);
        BUF_PUSH(sig->param_names, param->param.name);
    }

    hash_table_insert(&sema->fn_table, method_key, sig);
}

/**
 * Register each struct method as a fn sig in the sema's
 * fn table (keyed as "StructName.method_name").
 */
static void register_struct_methods(Sema *sema, const ASTNode *decl, StructDef *def) {
    for (int32_t i = 0; i < BUF_LEN(decl->struct_decl.methods); i++) {
        register_method_sig(sema, def->name, decl->struct_decl.methods[i], &def->methods);
    }
}

/** Build a single EnumVariant from its AST representation. */
static EnumVariant build_enum_variant(Sema *sema, ASTEnumVariant *ast_variant,
                                      int32_t *auto_discriminant) {
    EnumVariant variant = {0};
    variant.name = ast_variant->name;

    switch (ast_variant->kind) {
    case VARIANT_UNIT:
        variant.kind = ENUM_VARIANT_UNIT;
        break;
    case VARIANT_TUPLE: {
        variant.kind = ENUM_VARIANT_TUPLE;
        variant.tuple_count = BUF_LEN(ast_variant->tuple_types);
        variant.tuple_types = (const Type **)arena_alloc_zero(
            sema->arena, variant.tuple_count * sizeof(const Type *));
        for (int32_t j = 0; j < variant.tuple_count; j++) {
            variant.tuple_types[j] = resolve_ast_type(sema, &ast_variant->tuple_types[j]);
            if (variant.tuple_types[j] == NULL) {
                variant.tuple_types[j] = &TYPE_ERR_INST;
            }
        }
        break;
    }
    case VARIANT_STRUCT: {
        variant.kind = ENUM_VARIANT_STRUCT;
        variant.field_count = BUF_LEN(ast_variant->fields);
        variant.fields = arena_alloc_zero(sema->arena, variant.field_count * sizeof(StructField));
        for (int32_t j = 0; j < variant.field_count; j++) {
            variant.fields[j].name = ast_variant->fields[j].name;
            variant.fields[j].type = resolve_ast_type(sema, &ast_variant->fields[j].type);
            if (variant.fields[j].type == NULL) {
                variant.fields[j].type = &TYPE_ERR_INST;
            }
        }
        break;
    }
    }

    if (ast_variant->discriminant != NULL) {
        if (ast_variant->discriminant->kind == NODE_LIT &&
            ast_variant->discriminant->lit.kind == LIT_I32) {
            variant.discriminant = (int32_t)ast_variant->discriminant->lit.integer_value;
            *auto_discriminant = (int32_t)variant.discriminant + 1;
        }
    } else {
        variant.discriminant = *auto_discriminant;
        (*auto_discriminant)++;
    }
    return variant;
}

/**
 * Collect all required fields from a pact and its super pacts (recursively).
 * Appends to @p fields buf.
 */
static void collect_pact_fields(Sema *sema, const PactDef *pact, StructFieldInfo **fields,
                                SrcLoc loc) {
    (void)loc;
    // Recurse into super pacts
    for (int32_t i = 0; i < BUF_LEN(pact->super_pacts); i++) {
        PactDef *super = sema_lookup_pact(sema, pact->super_pacts[i]);
        if (super != NULL) {
            collect_pact_fields(sema, super, fields, loc);
        }
    }
    // Add this pact's own fields (avoid duplicates)
    for (int32_t i = 0; i < BUF_LEN(pact->fields); i++) {
        bool exists = false;
        for (int32_t j = 0; j < BUF_LEN(*fields); j++) {
            if (strcmp((*fields)[j].name, pact->fields[i].name) == 0) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            BUF_PUSH(*fields, pact->fields[i]);
        }
    }
}

/**
 * Collect all required methods from a pact and its super pacts (recursively).
 * Appends to @p methods buf.
 */
static void collect_pact_methods(Sema *sema, const PactDef *pact, StructMethodInfo **methods,
                                 SrcLoc loc) {
    (void)loc;
    for (int32_t i = 0; i < BUF_LEN(pact->super_pacts); i++) {
        PactDef *super = sema_lookup_pact(sema, pact->super_pacts[i]);
        if (super != NULL) {
            collect_pact_methods(sema, super, methods, loc);
        }
    }
    for (int32_t i = 0; i < BUF_LEN(pact->methods); i++) {
        bool exists = false;
        for (int32_t j = 0; j < BUF_LEN(*methods); j++) {
            if (strcmp((*methods)[j].name, pact->methods[i].name) == 0) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            BUF_PUSH(*methods, pact->methods[i]);
        }
    }
}

// ── Registration functions ─────────────────────────────────────────

void register_fn_sig(Sema *sema, ASTNode *decl) {
    // If the fn has type params, store as a generic template instead
    if (BUF_LEN(decl->fn_decl.type_params) > 0) {
        GenericFnDef *gdef = rsg_malloc(sizeof(*gdef));
        gdef->name = decl->fn_decl.name;
        gdef->decl = decl;
        gdef->type_params = decl->fn_decl.type_params;
        gdef->type_param_count = BUF_LEN(decl->fn_decl.type_params);
        hash_table_insert(&sema->generic_fn_table, decl->fn_decl.name, gdef);
        return;
    }

    const Type *resolved_return = &TYPE_UNIT_INST;
    if (decl->fn_decl.return_type.kind != AST_TYPE_INFERRED) {
        resolved_return = resolve_ast_type(sema, &decl->fn_decl.return_type);
        if (resolved_return == NULL) {
            resolved_return = &TYPE_UNIT_INST;
        }
    }

    FnSig *sig = rsg_malloc(sizeof(*sig));
    sig->name = decl->fn_decl.name;
    sig->return_type = resolved_return;
    sig->param_types = NULL;
    sig->param_names = NULL;
    sig->param_count = BUF_LEN(decl->fn_decl.params);
    sig->is_pub = decl->fn_decl.is_pub;
    for (int32_t j = 0; j < sig->param_count; j++) {
        ASTNode *param = decl->fn_decl.params[j];
        const Type *param_type = resolve_ast_type(sema, &param->param.type);
        if (param_type == NULL) {
            param_type = &TYPE_ERR_INST;
        }
        BUF_PUSH(sig->param_types, param_type);
        BUF_PUSH(sig->param_names, param->param.name);
    }
    hash_table_insert(&sema->fn_table, decl->fn_decl.name, sig);

    scope_define(sema,
                 &(SymDef){decl->fn_decl.name, resolved_return, decl->fn_decl.is_pub, SYM_FN});
}

void register_struct_def(Sema *sema, ASTNode *decl) {
    const char *struct_name = decl->struct_decl.name;

    // If the struct has type params, store as a generic template instead
    if (BUF_LEN(decl->struct_decl.type_params) > 0) {
        GenericStructDef *gdef = rsg_malloc(sizeof(*gdef));
        gdef->name = struct_name;
        gdef->decl = decl;
        gdef->type_params = decl->struct_decl.type_params;
        gdef->type_param_count = BUF_LEN(decl->struct_decl.type_params);
        hash_table_insert(&sema->generic_struct_table, struct_name, gdef);
        return;
    }

    // Check for duplicate struct def
    if (sema_lookup_struct(sema, struct_name) != NULL) {
        SEMA_ERR(sema, decl->loc, "duplicate struct def '%s'", struct_name);
        return;
    }

    StructDef *def = rsg_malloc(sizeof(*def));
    def->name = struct_name;
    def->fields = NULL;
    def->methods = NULL;
    def->embedded = NULL;

    // Collect embedded struct types
    for (int32_t i = 0; i < BUF_LEN(decl->struct_decl.embedded); i++) {
        BUF_PUSH(def->embedded, decl->struct_decl.embedded[i]);
    }

    compose_struct_fields(sema, decl, def);
    build_struct_type(sema, decl, def);
    register_struct_methods(sema, decl, def);

    hash_table_insert(&sema->struct_table, struct_name, def);

    // Register struct as a type alias so resolve_ast_type can find it
    hash_table_insert(&sema->type_alias_table, struct_name, (void *)def->type);

    // Register struct name as a type sym
    scope_define(sema, &(SymDef){struct_name, def->type, false, SYM_TYPE});
}

void register_enum_def(Sema *sema, ASTNode *decl) {
    const char *enum_name = decl->enum_decl.name;

    // If the enum has type params, store as a generic template instead
    if (BUF_LEN(decl->enum_decl.type_params) > 0) {
        GenericEnumDef *gdef = rsg_malloc(sizeof(*gdef));
        gdef->name = enum_name;
        gdef->decl = decl;
        gdef->type_params = decl->enum_decl.type_params;
        gdef->type_param_count = BUF_LEN(decl->enum_decl.type_params);
        hash_table_insert(&sema->generic_enum_table, enum_name, gdef);
        return;
    }

    if (sema_lookup_enum(sema, enum_name) != NULL) {
        SEMA_ERR(sema, decl->loc, "duplicate enum def '%s'", enum_name);
        return;
    }

    EnumVariant *variants = NULL;
    int32_t auto_discriminant = 0;
    for (int32_t i = 0; i < BUF_LEN(decl->enum_decl.variants); i++) {
        EnumVariant variant =
            build_enum_variant(sema, &decl->enum_decl.variants[i], &auto_discriminant);
        BUF_PUSH(variants, variant);
    }

    const Type *enum_type = type_create_enum(sema->arena, enum_name, variants, BUF_LEN(variants));
    decl->type = enum_type;

    EnumDef *def = rsg_malloc(sizeof(*def));
    def->name = enum_name;
    def->methods = NULL;
    def->type = enum_type;

    for (int32_t i = 0; i < BUF_LEN(decl->enum_decl.methods); i++) {
        register_method_sig(sema, enum_name, decl->enum_decl.methods[i], &def->methods);
    }

    hash_table_insert(&sema->enum_table, enum_name, def);
    hash_table_insert(&sema->type_alias_table, enum_name, (void *)enum_type);
    scope_define(sema, &(SymDef){enum_name, enum_type, false, SYM_TYPE});
}

void register_pact_def(Sema *sema, ASTNode *decl) {
    const char *pact_name = decl->pact_decl.name;

    if (sema_lookup_pact(sema, pact_name) != NULL) {
        SEMA_ERR(sema, decl->loc, "duplicate pact def '%s'", pact_name);
        return;
    }

    PactDef *def = rsg_malloc(sizeof(*def));
    def->name = pact_name;
    def->fields = NULL;
    def->methods = NULL;
    def->super_pacts = NULL;

    // Copy super pact refs
    for (int32_t i = 0; i < BUF_LEN(decl->pact_decl.super_pacts); i++) {
        BUF_PUSH(def->super_pacts, decl->pact_decl.super_pacts[i]);
    }

    // Register required fields
    for (int32_t i = 0; i < BUF_LEN(decl->pact_decl.fields); i++) {
        ASTStructField *ast_field = &decl->pact_decl.fields[i];
        const Type *field_type = resolve_ast_type(sema, &ast_field->type);
        if (field_type == NULL) {
            field_type = &TYPE_ERR_INST;
        }
        StructFieldInfo fi = {.name = ast_field->name, .type = field_type, .default_value = NULL};
        BUF_PUSH(def->fields, fi);
    }

    // Register methods
    for (int32_t i = 0; i < BUF_LEN(decl->pact_decl.methods); i++) {
        ASTNode *method = decl->pact_decl.methods[i];
        StructMethodInfo mi = {.name = method->fn_decl.name,
                               .is_mut_recv = method->fn_decl.is_mut_recv,
                               .is_ptr_recv = method->fn_decl.is_ptr_recv,
                               .recv_name = method->fn_decl.recv_name,
                               .decl = method};
        BUF_PUSH(def->methods, mi);
    }

    hash_table_insert(&sema->pact_table, pact_name, def);
}

void enforce_pact_conformances(Sema *sema, ASTNode *decl, StructDef *def) {
    for (int32_t ci = 0; ci < BUF_LEN(decl->struct_decl.conformances); ci++) {
        const char *pact_name = decl->struct_decl.conformances[ci];
        PactDef *pact = sema_lookup_pact(sema, pact_name);
        if (pact == NULL) {
            SEMA_ERR(sema, decl->loc, "unknown pact '%s'", pact_name);
            continue;
        }

        // Collect all required fields from this pact and its super pacts
        StructFieldInfo *required_fields = NULL;
        collect_pact_fields(sema, pact, &required_fields, decl->loc);

        // Check required fields exist in struct
        for (int32_t i = 0; i < BUF_LEN(required_fields); i++) {
            bool found = false;
            for (int32_t j = 0; j < BUF_LEN(def->fields); j++) {
                if (strcmp(def->fields[j].name, required_fields[i].name) == 0) {
                    if (!type_equal(def->fields[j].type, required_fields[i].type)) {
                        SEMA_ERR(sema, decl->loc,
                                 "field '%s' has type '%s' but pact '%s' requires type '%s'",
                                 required_fields[i].name,
                                 type_name(sema->arena, def->fields[j].type), pact_name,
                                 type_name(sema->arena, required_fields[i].type));
                    }
                    found = true;
                    break;
                }
            }
            if (!found) {
                SEMA_ERR(sema, decl->loc, "missing required field '%s' from pact '%s'",
                         required_fields[i].name, pact_name);
            }
        }
        BUF_FREE(required_fields);

        // Collect all required methods from this pact and its super pacts
        StructMethodInfo *pact_methods = NULL;
        collect_pact_methods(sema, pact, &pact_methods, decl->loc);

        // Check required methods exist or inject default implementations
        for (int32_t i = 0; i < BUF_LEN(pact_methods); i++) {
            bool has_body =
                pact_methods[i].decl != NULL && pact_methods[i].decl->fn_decl.body != NULL;

            // Check if struct already has this method
            bool found = false;
            for (int32_t j = 0; j < BUF_LEN(def->methods); j++) {
                if (strcmp(def->methods[j].name, pact_methods[i].name) == 0) {
                    found = true;
                    break;
                }
            }

            if (!found) {
                if (has_body) {
                    // Inject default method into struct
                    ASTNode *method_ast = pact_methods[i].decl;
                    method_ast->fn_decl.owner_struct = def->name;
                    BUF_PUSH(decl->struct_decl.methods, method_ast);
                    register_method_sig(sema, def->name, method_ast, &def->methods);
                } else {
                    SEMA_ERR(sema, decl->loc, "missing required method '%s' from pact '%s'",
                             pact_methods[i].name, pact_name);
                }
            }
        }
        BUF_FREE(pact_methods);
    }
}

void inject_builtin_enums(Sema *sema) {
    SrcLoc bltin = {.file = "<builtin>", .line = 0, .column = 0};

    // enum Option<T> { None, Some(T) }
    ASTNode *opt = ast_new(sema->arena, NODE_ENUM_DECL, bltin);
    opt->enum_decl.name = "Option";
    opt->enum_decl.variants = NULL;
    opt->enum_decl.methods = NULL;
    opt->enum_decl.type_params = NULL;

    ASTTypeParam tp_t = {.name = "T", .bounds = NULL};
    BUF_PUSH(opt->enum_decl.type_params, tp_t);

    ASTEnumVariant v_none = {.name = "None",
                             .kind = VARIANT_UNIT,
                             .tuple_types = NULL,
                             .fields = NULL,
                             .discriminant = NULL};
    BUF_PUSH(opt->enum_decl.variants, v_none);

    ASTType some_inner = {.kind = AST_TYPE_NAME, .name = "T", .type_args = NULL};
    ASTType *some_types = NULL;
    BUF_PUSH(some_types, some_inner);
    ASTEnumVariant v_some = {.name = "Some",
                             .kind = VARIANT_TUPLE,
                             .tuple_types = some_types,
                             .fields = NULL,
                             .discriminant = NULL};
    BUF_PUSH(opt->enum_decl.variants, v_some);

    register_enum_def(sema, opt);

    // enum Result<T, E> { Ok(T), Err(E) }
    ASTNode *res = ast_new(sema->arena, NODE_ENUM_DECL, bltin);
    res->enum_decl.name = "Result";
    res->enum_decl.variants = NULL;
    res->enum_decl.methods = NULL;
    res->enum_decl.type_params = NULL;

    ASTTypeParam tp_t2 = {.name = "T", .bounds = NULL};
    ASTTypeParam tp_e = {.name = "E", .bounds = NULL};
    BUF_PUSH(res->enum_decl.type_params, tp_t2);
    BUF_PUSH(res->enum_decl.type_params, tp_e);

    ASTType ok_inner = {.kind = AST_TYPE_NAME, .name = "T", .type_args = NULL};
    ASTType *ok_types = NULL;
    BUF_PUSH(ok_types, ok_inner);
    ASTEnumVariant v_ok = {.name = "Ok",
                           .kind = VARIANT_TUPLE,
                           .tuple_types = ok_types,
                           .fields = NULL,
                           .discriminant = NULL};
    BUF_PUSH(res->enum_decl.variants, v_ok);

    ASTType err_inner = {.kind = AST_TYPE_NAME, .name = "E", .type_args = NULL};
    ASTType *err_types = NULL;
    BUF_PUSH(err_types, err_inner);
    ASTEnumVariant v_err = {.name = "Err",
                            .kind = VARIANT_TUPLE,
                            .tuple_types = err_types,
                            .fields = NULL,
                            .discriminant = NULL};
    BUF_PUSH(res->enum_decl.variants, v_err);

    register_enum_def(sema, res);
}
