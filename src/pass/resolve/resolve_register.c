#include "_sema.h"

#ifdef _WIN32
#define PATH_SEP '\\'
#else
#define PATH_SEP '/'
#endif

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
            StructFieldInfo fi = {
                .name = ef->name, .type = ef->type, .default_value = NULL, .is_pub = ef->is_pub};
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
                                  .default_value = ast_field->default_value,
                                  .is_pub = ast_field->is_pub};
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
            StructField sf = {.name = def->embedded[i], .type = embed_def->type, .is_pub = true};
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
        StructField sf = {.name = ast_field->name, .type = field_type, .is_pub = ast_field->is_pub};
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
 * Create a FnSig from a fn decl, resolving its return type and param types.
 * The caller is responsible for inserting the sig into the fn table.
 */
FnSig *build_fn_sig(Sema *sema, ASTNode *decl, bool is_pub) {
    const Type *return_type = &TYPE_UNIT_INST;
    if (decl->fn_decl.return_type.kind != AST_TYPE_INFERRED) {
        return_type = resolve_ast_type(sema, &decl->fn_decl.return_type);
        if (return_type == NULL) {
            return_type = &TYPE_UNIT_INST;
        }
    }

    FnSig *sig = rsg_malloc(sizeof(*sig));
    sig->name = decl->fn_decl.name;
    sig->return_type = return_type;
    sig->param_types = NULL;
    sig->param_names = NULL;
    sig->param_count = BUF_LEN(decl->fn_decl.params);
    sig->is_pub = is_pub;
    sig->has_variadic = false;

    for (int32_t j = 0; j < sig->param_count; j++) {
        ASTNode *param = decl->fn_decl.params[j];
        const Type *pt = resolve_ast_type(sema, &param->param.type);
        if (pt == NULL) {
            pt = &TYPE_ERR_INST;
        }
        // Variadic param: ..T → []T (slice type)
        if (param->param.is_variadic) {
            pt = type_create_slice(sema->arena, pt);
            sig->has_variadic = true;
        }
        BUF_PUSH(sig->param_types, pt);
        BUF_PUSH(sig->param_names, param->param.name);
    }
    return sig;
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
    FnSig *sig = build_fn_sig(sema, method, false);
    sig->is_ptr_recv = method->fn_decl.is_ptr_recv;
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
 * Appends to @p fields buf, deduplicating by name.
 */
static void collect_pact_fields(Sema *sema, const PactDef *pact, StructFieldInfo **fields) {
    for (int32_t i = 0; i < BUF_LEN(pact->super_pacts); i++) {
        PactDef *super = sema_lookup_pact(sema, pact->super_pacts[i]);
        if (super != NULL) {
            collect_pact_fields(sema, super, fields);
        }
    }
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
 * Appends to @p methods buf, deduplicating by name.
 */
static void collect_pact_methods(Sema *sema, const PactDef *pact, StructMethodInfo **methods) {
    for (int32_t i = 0; i < BUF_LEN(pact->super_pacts); i++) {
        PactDef *super = sema_lookup_pact(sema, pact->super_pacts[i]);
        if (super != NULL) {
            collect_pact_methods(sema, super, methods);
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
        // Reject default generics on functions
        for (int32_t i = 0; i < BUF_LEN(decl->fn_decl.type_params); i++) {
            if (decl->fn_decl.type_params[i].default_type != NULL) {
                SEMA_ERR(sema, decl->loc, "default generics are not allowed on fn declarations");
                return;
            }
        }
        GenericFnDef *gdef = rsg_malloc(sizeof(*gdef));
        gdef->name = decl->fn_decl.name;
        gdef->decl = decl;
        gdef->type_params = decl->fn_decl.type_params;
        gdef->type_param_count = BUF_LEN(decl->fn_decl.type_params);
        hash_table_insert(&sema->generics.fn, decl->fn_decl.name, gdef);
        return;
    }

    FnSig *sig = build_fn_sig(sema, decl, decl->fn_decl.is_pub);
    hash_table_insert(&sema->fn_table, decl->fn_decl.name, sig);

    scope_define(sema,
                 &(SymDef){decl->fn_decl.name, sig->return_type, decl->fn_decl.is_pub, SYM_FN});
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
        hash_table_insert(&sema->generics.structs, struct_name, gdef);
        return;
    }

    // Check for duplicate struct def
    if (sema_lookup_struct(sema, struct_name) != NULL) {
        SEMA_ERR(sema, decl->loc, "duplicate struct def '%s'", struct_name);
        return;
    }

    const char *prev_self = sema->self_type_name;
    sema->self_type_name = struct_name;

    StructDef *def = rsg_malloc(sizeof(*def));
    def->name = struct_name;
    def->is_tuple_struct = decl->struct_decl.is_tuple_struct;
    def->fields = NULL;
    def->methods = NULL;
    def->embedded = NULL;
    def->assoc_types = NULL;

    // Copy associated types from AST
    for (int32_t i = 0; i < BUF_LEN(decl->struct_decl.assoc_types); i++) {
        BUF_PUSH(def->assoc_types, decl->struct_decl.assoc_types[i]);
    }

    // Collect embedded struct types
    for (int32_t i = 0; i < BUF_LEN(decl->struct_decl.embedded); i++) {
        BUF_PUSH(def->embedded, decl->struct_decl.embedded[i]);
    }

    compose_struct_fields(sema, decl, def);
    build_struct_type(sema, decl, def);

    // Register type alias early so Self-referencing method sigs can resolve
    hash_table_insert(&sema->struct_table, struct_name, def);
    hash_table_insert(&sema->type_alias_table, struct_name, (void *)def->type);
    scope_define(sema, &(SymDef){struct_name, def->type, false, SYM_TYPE});

    register_struct_methods(sema, decl, def);

    sema->self_type_name = prev_self;
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
        hash_table_insert(&sema->generics.enums, enum_name, gdef);
        return;
    }

    if (sema_lookup_enum(sema, enum_name) != NULL) {
        SEMA_ERR(sema, decl->loc, "duplicate enum def '%s'", enum_name);
        return;
    }

    const char *prev_self = sema->self_type_name;
    sema->self_type_name = enum_name;

    // Register associated type aliases before variant resolution
    // so that variant types can reference them (e.g., Data(Payload))
    for (int32_t i = 0; i < BUF_LEN(decl->enum_decl.assoc_types); i++) {
        if (decl->enum_decl.assoc_types[i].concrete_type != NULL) {
            const Type *at_type =
                resolve_ast_type(sema, decl->enum_decl.assoc_types[i].concrete_type);
            if (at_type != NULL && at_type->kind != TYPE_ERR) {
                hash_table_insert(&sema->type_alias_table, decl->enum_decl.assoc_types[i].name,
                                  (void *)at_type);
            }
        }
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
    def->assoc_types = NULL;
    def->type = enum_type;

    // Copy associated types from AST
    for (int32_t i = 0; i < BUF_LEN(decl->enum_decl.assoc_types); i++) {
        BUF_PUSH(def->assoc_types, decl->enum_decl.assoc_types[i]);
    }

    // Register type alias early so Self-referencing method sigs can resolve
    hash_table_insert(&sema->enum_table, enum_name, def);
    hash_table_insert(&sema->type_alias_table, enum_name, (void *)enum_type);
    scope_define(sema, &(SymDef){enum_name, enum_type, false, SYM_TYPE});

    for (int32_t i = 0; i < BUF_LEN(decl->enum_decl.methods); i++) {
        register_method_sig(sema, enum_name, decl->enum_decl.methods[i], &def->methods);
    }

    sema->self_type_name = prev_self;
}

void register_pact_def(Sema *sema, ASTNode *decl) {
    const char *pact_name = decl->pact_decl.name;

    if (sema_lookup_pact(sema, pact_name) != NULL) {
        SEMA_ERR(sema, decl->loc, "duplicate pact def '%s'", pact_name);
        return;
    }

    const char *prev_self = sema->self_type_name;
    sema->self_type_name = pact_name;

    PactDef *def = rsg_malloc(sizeof(*def));
    def->name = pact_name;
    def->fields = NULL;
    def->methods = NULL;
    def->super_pacts = NULL;
    def->assoc_types = NULL;

    // Copy associated types from AST
    for (int32_t i = 0; i < BUF_LEN(decl->pact_decl.assoc_types); i++) {
        BUF_PUSH(def->assoc_types, decl->pact_decl.assoc_types[i]);
    }

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
        StructFieldInfo fi = {
            .name = ast_field->name, .type = field_type, .default_value = NULL, .is_pub = true};
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
    sema->self_type_name = prev_self;
}

/** Verify that all pact-required fields are present and type-correct. */
static void enforce_pact_fields(Sema *sema, const ASTNode *decl, const StructDef *def,
                                PactDef *pact) {
    StructFieldInfo *required_fields = NULL;
    collect_pact_fields(sema, pact, &required_fields);

    for (int32_t i = 0; i < BUF_LEN(required_fields); i++) {
        bool found = false;
        for (int32_t j = 0; j < BUF_LEN(def->fields); j++) {
            if (strcmp(def->fields[j].name, required_fields[i].name) == 0) {
                if (!type_equal(def->fields[j].type, required_fields[i].type)) {
                    SEMA_ERR(sema, decl->loc,
                             "field '%s' has type '%s' but pact '%s' requires type '%s'",
                             required_fields[i].name, type_name(sema->arena, def->fields[j].type),
                             pact->name, type_name(sema->arena, required_fields[i].type));
                }
                found = true;
                break;
            }
        }
        if (!found) {
            SEMA_ERR(sema, decl->loc, "missing required field '%s' from pact '%s'",
                     required_fields[i].name, pact->name);
        }
    }
    BUF_FREE(required_fields);
}

/** Verify pact-required methods exist; inject defaults when available. */
static void enforce_pact_methods(Sema *sema, ASTNode *decl, StructDef *def, PactDef *pact) {
    StructMethodInfo *pact_methods = NULL;
    collect_pact_methods(sema, pact, &pact_methods);

    for (int32_t i = 0; i < BUF_LEN(pact_methods); i++) {
        bool has_body = pact_methods[i].decl != NULL && pact_methods[i].decl->fn_decl.body != NULL;

        bool found = false;
        for (int32_t j = 0; j < BUF_LEN(def->methods); j++) {
            if (strcmp(def->methods[j].name, pact_methods[i].name) == 0) {
                found = true;
                break;
            }
        }

        if (!found) {
            if (has_body) {
                ASTNode *method_ast = pact_methods[i].decl;
                method_ast->fn_decl.owner_struct = def->name;
                BUF_PUSH(decl->struct_decl.methods, method_ast);
                register_method_sig(sema, def->name, method_ast, &def->methods);
            } else {
                SEMA_ERR(sema, decl->loc, "missing required method '%s' from pact '%s'",
                         pact_methods[i].name, pact->name);
            }
        }
    }
    BUF_FREE(pact_methods);
}

/** Apply defaults for missing assoc types and reject unknown ones. */
static void validate_pact_assoc_types(Sema *sema, ASTNode *decl, StructDef *def,
                                      const PactDef *pact, const char *pact_name) {
    // Check required associated types — apply defaults when available
    for (int32_t i = 0; i < BUF_LEN(pact->assoc_types); i++) {
        const char *at_name = pact->assoc_types[i].name;
        bool found = false;
        for (int32_t j = 0; j < BUF_LEN(def->assoc_types); j++) {
            if (strcmp(def->assoc_types[j].name, at_name) != 0) {
                continue;
            }
            if (def->assoc_types[j].pact_qualifier != NULL &&
                strcmp(def->assoc_types[j].pact_qualifier, pact_name) != 0) {
                continue;
            }
            found = true;
            break;
        }
        if (!found) {
            if (pact->assoc_types[i].concrete_type != NULL) {
                ASTAssocType defaulted = {
                    .name = at_name,
                    .pact_qualifier = NULL,
                    .bounds = NULL,
                    .concrete_type = pact->assoc_types[i].concrete_type,
                };
                BUF_PUSH(def->assoc_types, defaulted);
                BUF_PUSH(decl->struct_decl.assoc_types, defaulted);
            } else {
                SEMA_ERR(sema, decl->loc, "missing associated type '%s' required by pact '%s'",
                         at_name, pact_name);
            }
        }
    }
    // Check struct doesn't define associated types not in the pact
    for (int32_t j = 0; j < BUF_LEN(def->assoc_types); j++) {
        const char *at_name = def->assoc_types[j].name;
        if (def->assoc_types[j].pact_qualifier != NULL &&
            strcmp(def->assoc_types[j].pact_qualifier, pact_name) != 0) {
            continue;
        }
        bool in_pact = false;
        for (int32_t i = 0; i < BUF_LEN(pact->assoc_types); i++) {
            if (strcmp(pact->assoc_types[i].name, at_name) == 0) {
                in_pact = true;
                break;
            }
        }
        if (!in_pact) {
            SEMA_ERR(sema, decl->loc, "associated type '%s' is not a member of pact '%s'", at_name,
                     pact_name);
        }
    }
}

/** Enforce pact-declared bounds on each associated type. */
static void enforce_pact_assoc_type_bounds(Sema *sema, ASTNode *decl, StructDef *def,
                                           const PactDef *pact) {
    for (int32_t i = 0; i < BUF_LEN(pact->assoc_types); i++) {
        if (BUF_LEN(pact->assoc_types[i].bounds) == 0) {
            continue;
        }
        const char *at_name = pact->assoc_types[i].name;
        for (int32_t j = 0; j < BUF_LEN(def->assoc_types); j++) {
            if (strcmp(def->assoc_types[j].name, at_name) != 0) {
                continue;
            }
            if (def->assoc_types[j].pact_qualifier != NULL &&
                strcmp(def->assoc_types[j].pact_qualifier, pact->name) != 0) {
                continue;
            }
            if (def->assoc_types[j].concrete_type == NULL) {
                break;
            }
            const Type *concrete = resolve_ast_type(sema, def->assoc_types[j].concrete_type);
            if (concrete == NULL || concrete->kind == TYPE_ERR) {
                break;
            }
            for (int32_t b = 0; b < BUF_LEN(pact->assoc_types[i].bounds); b++) {
                const char *bound = pact->assoc_types[i].bounds[b];
                if (!type_satisfies_bound(sema, concrete, bound)) {
                    SEMA_ERR(sema, decl->loc,
                             "associated type '%s' = '%s' does not satisfy bound '%s'", at_name,
                             type_name(sema->arena, concrete), bound);
                }
            }
            break;
        }
    }
}

void enforce_pact_conformances(Sema *sema, ASTNode *decl, StructDef *def) {
    for (int32_t ci = 0; ci < BUF_LEN(decl->struct_decl.conformances); ci++) {
        const char *pact_name = decl->struct_decl.conformances[ci];
        PactDef *pact = sema_lookup_pact(sema, pact_name);
        if (pact == NULL) {
            SEMA_ERR(sema, decl->loc, "unknown pact '%s'", pact_name);
            continue;
        }
        validate_pact_assoc_types(sema, decl, def, pact, pact_name);
        enforce_pact_assoc_type_bounds(sema, decl, def, pact);
        enforce_pact_fields(sema, decl, def, pact);
        enforce_pact_methods(sema, decl, def, pact);
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
    opt->enum_decl.assoc_types = NULL;

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
    res->enum_decl.assoc_types = NULL;

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

// ── Module registration ─────────────────────────────────────────────

void register_module_decl(Sema *sema, ASTNode *decl) {
    const char *mod_name = decl->module.name;

    // Create module type and register in scope
    const Type *mod_type = type_create_module(sema->arena, mod_name);
    scope_define(sema, &(SymDef){mod_name, mod_type, true, SYM_MODULE});

    if (decl->module.decls == NULL) {
        return;
    }

    // Set module context so resolve_ast_type can find sibling types
    const char *prev_module_name = sema->current_scope->module_name;
    sema->current_scope->module_name = mod_name;

    // Register all inner declarations with qualified names
    for (int32_t i = 0; i < BUF_LEN(decl->module.decls); i++) {
        ASTNode *inner = decl->module.decls[i];

        if (inner->kind == NODE_MODULE) {
            // Nested sub-module: prefix its name and recurse
            const char *orig_name = inner->module.name;
            inner->module.name = arena_sprintf(sema->arena, "%s.%s", mod_name, orig_name);

            // Filesystem module: load from module_search_dir/<orig_name>.rsg
            if (inner->module.decls == NULL && sema->module_search_dir != NULL) {
                const char *mod_path = arena_sprintf(sema->arena, "%s%c%s.rsg",
                                                     sema->module_search_dir, PATH_SEP, orig_name);
                ASTNode **decls = load_module_decls(sema, mod_path);
                if (decls == NULL) {
                    SEMA_ERR(sema, inner->loc, "cannot find module file '%s'", mod_path);
                } else {
                    inner->module.decls = decls;
                    // Update search dir for grandchild modules
                    const char *prev_dir = sema->module_search_dir;
                    sema->module_search_dir =
                        arena_sprintf(sema->arena, "%s%c%s", prev_dir, PATH_SEP, orig_name);
                    register_module_decl(sema, inner);
                    sema->module_search_dir = prev_dir;
                    continue;
                }
            }

            register_module_decl(sema, inner);
            continue;
        }

        if (inner->kind == NODE_FN_DECL) {
            // Rewrite fn name to qualified form: mod.fn_name
            const char *qualified =
                arena_sprintf(sema->arena, "%s.%s", mod_name, inner->fn_decl.name);
            inner->fn_decl.name = qualified;
            register_fn_sig(sema, inner);
            continue;
        }

        if (inner->kind == NODE_STRUCT_DECL) {
            const char *qualified =
                arena_sprintf(sema->arena, "%s.%s", mod_name, inner->struct_decl.name);
            inner->struct_decl.name = qualified;
            register_struct_def(sema, inner);
            continue;
        }

        if (inner->kind == NODE_ENUM_DECL) {
            const char *qualified =
                arena_sprintf(sema->arena, "%s.%s", mod_name, inner->enum_decl.name);
            inner->enum_decl.name = qualified;
            register_enum_def(sema, inner);
            continue;
        }

        if (inner->kind == NODE_PACT_DECL) {
            const char *qualified =
                arena_sprintf(sema->arena, "%s.%s", mod_name, inner->pact_decl.name);
            inner->pact_decl.name = qualified;
            register_pact_def(sema, inner);
            continue;
        }

        if (inner->kind == NODE_TYPE_ALIAS) {
            const char *qualified =
                arena_sprintf(sema->arena, "%s.%s", mod_name, inner->type_alias.name);
            inner->type_alias.name = qualified;
            const Type *underlying = resolve_ast_type(sema, &inner->type_alias.alias_type);
            if (underlying != NULL) {
                hash_table_insert(&sema->type_alias_table, qualified, (void *)underlying);
            }
            continue;
        }

        // Defer use decls — processed after all module contents are registered
    }

    // Process use decls inside the module (after all sibling decls are registered)
    for (int32_t i = 0; i < BUF_LEN(decl->module.decls); i++) {
        ASTNode *inner = decl->module.decls[i];
        if (inner->kind == NODE_USE_DECL) {
            register_use_decl(sema, inner);
        }
    }

    sema->current_scope->module_name = prev_module_name;
}

/** Resolve a 'super' or 'super::super' path to the parent module's qualified name. */
static const char *resolve_super_path(Sema *sema, const char *path, SrcLoc loc) {
    const char *current = sema->current_scope->module_name;
    if (current == NULL) {
        // At file scope, super:: has no parent
        SEMA_ERR(sema, loc, "'super' used outside of a module");
        return NULL;
    }
    // Walk up one level for each 'super' segment (separated by '::')
    // The path is "super" or "super::super" etc.
    const char *remaining = path;
    char *cur = arena_sprintf(sema->arena, "%s", current);
    while (remaining != NULL) {
        const char *sep = strstr(remaining, "::");
        size_t seg_len = (sep != NULL) ? (size_t)(sep - remaining) : strlen(remaining);
        if (seg_len == 5 && strncmp(remaining, "super", 5) == 0) {
            // Strip last component from cur (e.g., "a.b" -> "a")
            char *last_dot = strrchr(cur, '.');
            if (last_dot != NULL) {
                *last_dot = '\0';
            } else {
                // Already at top level — super goes to file scope
                return "";
            }
        } else {
            break;
        }
        remaining = (sep != NULL) ? sep + 2 : NULL;
    }
    return cur;
}

void register_use_decl(Sema *sema, ASTNode *decl) {
    const char *mod_name = decl->use_decl.module_path;

    // Resolve super:: paths first (before normalizing separators)
    if (strncmp(mod_name, "super", 5) == 0) {
        const char *resolved = resolve_super_path(sema, mod_name, decl->loc);
        if (resolved == NULL) {
            return;
        }
        mod_name = resolved;
        decl->use_decl.module_path = resolved;
    }

    // Strip leading self:: prefix (refers to current crate/module root)
    if (strncmp(mod_name, "self::", 6) == 0) {
        mod_name = mod_name + 6;
        decl->use_decl.module_path = mod_name;
    }

    // Normalize :: separators to . for internal lookup
    if (strstr(mod_name, "::") != NULL) {
        size_t len = strlen(mod_name);
        char *normalized = arena_alloc(sema->arena, len + 1);
        size_t j = 0;
        for (size_t i = 0; i < len; i++) {
            if (mod_name[i] == ':' && i + 1 < len && mod_name[i + 1] == ':') {
                normalized[j++] = '.';
                i++; // skip second ':'
            } else {
                normalized[j++] = mod_name[i];
            }
        }
        normalized[j] = '\0';
        mod_name = normalized;
        decl->use_decl.module_path = normalized;
    }

    for (int32_t i = 0; i < BUF_LEN(decl->use_decl.imported_names); i++) {
        const char *name = decl->use_decl.imported_names[i];
        const char *alias = decl->use_decl.aliases[i];
        const char *qualified;
        if (strlen(mod_name) == 0) {
            qualified = name;
        } else {
            qualified = arena_sprintf(sema->arena, "%s.%s", mod_name, name);
        }

        // Try fn
        FnSig *sig = sema_lookup_fn(sema, qualified);
        if (sig != NULL) {
            // Check visibility: fn must be pub
            if (!sig->is_pub) {
                SEMA_ERR(sema, decl->loc, "'%s' is private in module '%s'", name, mod_name);
                continue;
            }
            hash_table_insert(&sema->fn_table, alias, sig);
            continue;
        }

        // Try struct
        StructDef *sdef = sema_lookup_struct(sema, qualified);
        if (sdef != NULL) {
            hash_table_insert(&sema->struct_table, alias, sdef);
            scope_define(sema, &(SymDef){alias, sdef->type, true, SYM_TYPE});
            continue;
        }

        // Try enum
        EnumDef *edef = sema_lookup_enum(sema, qualified);
        if (edef != NULL) {
            hash_table_insert(&sema->enum_table, alias, edef);
            scope_define(sema, &(SymDef){alias, edef->type, true, SYM_TYPE});
            continue;
        }

        // Try type alias
        const Type *talias = sema_lookup_type_alias(sema, qualified);
        if (talias != NULL) {
            hash_table_insert(&sema->type_alias_table, alias, (void *)talias);
            continue;
        }

        SEMA_ERR(sema, decl->loc, "'%s' not found in module '%s'", name, mod_name);
    }
}

// ── Extension method registration ──────────────────────────────────

/** Push ext-declared associated types into @p target_buf and register as type aliases. */
static void register_ext_assoc_types(Sema *sema, const ASTNode *decl, ASTAssocType **target_buf) {
    for (int32_t i = 0; i < BUF_LEN(decl->ext_decl.assoc_types); i++) {
        BUF_PUSH(*target_buf, decl->ext_decl.assoc_types[i]);
        if (decl->ext_decl.assoc_types[i].concrete_type != NULL) {
            const Type *at_type =
                resolve_ast_type(sema, decl->ext_decl.assoc_types[i].concrete_type);
            if (at_type != NULL && at_type->kind != TYPE_ERR) {
                hash_table_insert(&sema->type_alias_table, decl->ext_decl.assoc_types[i].name,
                                  (void *)at_type);
            }
        }
    }
}

/** Register ext methods into the fn table and append to @p methods buf. */
static void register_ext_methods(Sema *sema, const char *target_name, const ASTNode *decl,
                                 StructMethodInfo **methods) {
    for (int32_t i = 0; i < BUF_LEN(decl->ext_decl.methods); i++) {
        ASTNode *method = decl->ext_decl.methods[i];
        method->fn_decl.owner_struct = target_name;
        register_method_sig(sema, target_name, method, methods);
    }
}

/** Register a single primitive ext method into the fn_table with "type.method" key. */
static void register_primitive_method(Sema *sema, const char *type_name, ASTNode *method) {
    method->fn_decl.owner_struct = type_name;

    const char *method_key = arena_sprintf(sema->arena, "%s.%s", type_name, method->fn_decl.name);
    FnSig *sig = build_fn_sig(sema, method, false);
    sig->is_ptr_recv = method->fn_decl.is_ptr_recv;
    hash_table_insert(&sema->fn_table, method_key, sig);
}

void register_ext_decl(Sema *sema, ASTNode *decl) {
    const char *target_name = decl->ext_decl.target_name;

    // For generic ext blocks (ext<T,U> Pair<T,U>), store template for later
    if (BUF_LEN(decl->ext_decl.type_params) > 0) {
        GenericExtDef *gext = rsg_malloc(sizeof(*gext));
        gext->base.name = target_name;
        gext->base.decl = decl;
        gext->base.type_params = decl->ext_decl.type_params;
        gext->base.type_param_count = BUF_LEN(decl->ext_decl.type_params);
        gext->target_name = target_name;
        BUF_PUSH(sema->generic_ext_defs, gext);
        return;
    }

    // Try to find the target as a struct
    StructDef *sdef = sema_lookup_struct(sema, target_name);
    if (sdef != NULL) {
        const char *prev_self = sema->self_type_name;
        sema->self_type_name = target_name;
        register_ext_assoc_types(sema, decl, &sdef->assoc_types);
        register_ext_methods(sema, target_name, decl, &sdef->methods);
        sema->self_type_name = prev_self;
        return;
    }

    // Try to find the target as an enum
    EnumDef *edef = sema_lookup_enum(sema, target_name);
    if (edef != NULL) {
        const char *prev_self = sema->self_type_name;
        sema->self_type_name = target_name;
        register_ext_assoc_types(sema, decl, &edef->assoc_types);
        register_ext_methods(sema, target_name, decl, &edef->methods);
        sema->self_type_name = prev_self;
        return;
    }

    // Primitive type
    for (int32_t i = 0; i < BUF_LEN(decl->ext_decl.methods); i++) {
        register_primitive_method(sema, target_name, decl->ext_decl.methods[i]);
    }
}

/** Check whether a pact method exists for an enum/primitive target. */
static bool ext_method_exists(const Sema *sema, const char *target_name, const char *method_name,
                              const EnumDef *edef) {
    if (edef != NULL) {
        for (int32_t j = 0; j < BUF_LEN(edef->methods); j++) {
            if (strcmp(edef->methods[j].name, method_name) == 0) {
                return true;
            }
        }
        return false;
    }
    // Primitive — check fn_table
    const char *key = arena_sprintf(sema->arena, "%s.%s", target_name, method_name);
    return sema_lookup_fn(sema, key) != NULL;
}

/** Enforce a single pact conformance for a struct-targeted ext decl. */
static void enforce_ext_struct_pact(Sema *sema, ASTNode *decl, StructDef *sdef,
                                    const char *pact_name, PactDef *pact) {
    // Apply defaults for missing associated types
    for (int32_t i = 0; i < BUF_LEN(pact->assoc_types); i++) {
        const char *at_name = pact->assoc_types[i].name;
        bool found = false;
        for (int32_t j = 0; j < BUF_LEN(sdef->assoc_types); j++) {
            if (strcmp(sdef->assoc_types[j].name, at_name) != 0) {
                continue;
            }
            if (sdef->assoc_types[j].pact_qualifier != NULL &&
                strcmp(sdef->assoc_types[j].pact_qualifier, pact_name) != 0) {
                continue;
            }
            found = true;
            break;
        }
        if (!found) {
            if (pact->assoc_types[i].concrete_type != NULL) {
                ASTAssocType defaulted = {
                    .name = at_name,
                    .pact_qualifier = NULL,
                    .bounds = NULL,
                    .concrete_type = pact->assoc_types[i].concrete_type,
                };
                BUF_PUSH(sdef->assoc_types, defaulted);
            } else {
                SEMA_ERR(sema, decl->loc, "missing associated type '%s' required by pact '%s'",
                         at_name, pact_name);
            }
        }
    }
    // Reject ext-declared assoc types not present in the pact
    for (int32_t j = 0; j < BUF_LEN(decl->ext_decl.assoc_types); j++) {
        const char *at_name = decl->ext_decl.assoc_types[j].name;
        if (decl->ext_decl.assoc_types[j].pact_qualifier != NULL &&
            strcmp(decl->ext_decl.assoc_types[j].pact_qualifier, pact_name) != 0) {
            continue;
        }
        bool in_pact = false;
        for (int32_t i = 0; i < BUF_LEN(pact->assoc_types); i++) {
            if (strcmp(pact->assoc_types[i].name, at_name) == 0) {
                in_pact = true;
                break;
            }
        }
        if (!in_pact) {
            SEMA_ERR(sema, decl->loc, "associated type '%s' is not a member of pact '%s'", at_name,
                     pact_name);
        }
    }
    // Reuse shared bounds enforcement
    enforce_pact_assoc_type_bounds(sema, decl, sdef, pact);

    // Enforce required methods and inject defaults
    StructMethodInfo *pact_methods = NULL;
    collect_pact_methods(sema, pact, &pact_methods);
    for (int32_t i = 0; i < BUF_LEN(pact_methods); i++) {
        bool found = false;
        for (int32_t j = 0; j < BUF_LEN(sdef->methods); j++) {
            if (strcmp(sdef->methods[j].name, pact_methods[i].name) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            bool has_body =
                pact_methods[i].decl != NULL && pact_methods[i].decl->fn_decl.body != NULL;
            if (has_body) {
                ASTNode *method_ast = pact_methods[i].decl;
                method_ast->fn_decl.owner_struct = sdef->name;
                BUF_PUSH(decl->ext_decl.methods, method_ast);
                register_method_sig(sema, sdef->name, method_ast, &sdef->methods);
            } else {
                SEMA_ERR(sema, decl->loc, "missing required method '%s' from pact '%s'",
                         pact_methods[i].name, pact_name);
            }
        }
    }
    BUF_FREE(pact_methods);
}

void enforce_ext_pact_conformances(Sema *sema, ASTNode *decl) {
    if (BUF_LEN(decl->ext_decl.impl_pacts) == 0) {
        return;
    }

    const char *target_name = decl->ext_decl.target_name;

    // For struct targets, delegate per-pact enforcement
    StructDef *sdef = sema_lookup_struct(sema, target_name);
    if (sdef != NULL) {
        for (int32_t pi = 0; pi < BUF_LEN(decl->ext_decl.impl_pacts); pi++) {
            const char *pact_name = decl->ext_decl.impl_pacts[pi];
            PactDef *pact = sema_lookup_pact(sema, pact_name);
            if (pact == NULL) {
                SEMA_ERR(sema, decl->loc, "unknown pact '%s'", pact_name);
                continue;
            }
            enforce_ext_struct_pact(sema, decl, sdef, pact_name, pact);
        }
        return;
    }

    // For enum/primitive targets, check methods exist in fn_table
    EnumDef *edef = sema_lookup_enum(sema, target_name);
    for (int32_t pi = 0; pi < BUF_LEN(decl->ext_decl.impl_pacts); pi++) {
        const char *pact_name = decl->ext_decl.impl_pacts[pi];
        PactDef *pact = sema_lookup_pact(sema, pact_name);
        if (pact == NULL) {
            SEMA_ERR(sema, decl->loc, "unknown pact '%s'", pact_name);
            continue;
        }

        StructMethodInfo *pact_methods = NULL;
        collect_pact_methods(sema, pact, &pact_methods);

        for (int32_t i = 0; i < BUF_LEN(pact_methods); i++) {
            if (!ext_method_exists(sema, target_name, pact_methods[i].name, edef)) {
                bool has_body =
                    pact_methods[i].decl != NULL && pact_methods[i].decl->fn_decl.body != NULL;
                if (!has_body) {
                    SEMA_ERR(sema, decl->loc, "missing required method '%s' from pact '%s'",
                             pact_methods[i].name, pact_name);
                }
            }
        }
        BUF_FREE(pact_methods);
    }
}
