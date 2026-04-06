#include "_sema.h"

// ── Public API ─────────────────────────────────────────────────────────

Sema *sema_create(Arena *arena) {
    Sema *analyzer = rsg_malloc(sizeof(*analyzer));
    analyzer->arena = arena;
    analyzer->current_scope = NULL;
    analyzer->err_count = 0;
    analyzer->loop_break_type = NULL;
    hash_table_init(&analyzer->type_alias_table, NULL);
    hash_table_init(&analyzer->fn_table, NULL);
    hash_table_init(&analyzer->struct_table, NULL);
    hash_table_init(&analyzer->enum_table, NULL);
    hash_table_init(&analyzer->pact_table, NULL);
    return analyzer;
}

void sema_destroy(Sema *analyzer) {
    if (analyzer != NULL) {
        hash_table_destroy(&analyzer->type_alias_table);
        hash_table_destroy(&analyzer->fn_table);
        hash_table_destroy(&analyzer->struct_table);
        hash_table_destroy(&analyzer->enum_table);
        hash_table_destroy(&analyzer->pact_table);
        free(analyzer);
    }
}

/** Register a single fn's signature into the analyzer tables and scope. */
static void register_fn_signature(Sema *analyzer, ASTNode *decl) {
    const Type *resolved_return = &TYPE_UNIT_INST;
    if (decl->fn_decl.return_type.kind != AST_TYPE_INFERRED) {
        resolved_return = resolve_ast_type(analyzer, &decl->fn_decl.return_type);
        if (resolved_return == NULL) {
            resolved_return = &TYPE_UNIT_INST;
        }
    }

    FnSignature *signature = rsg_malloc(sizeof(*signature));
    signature->name = decl->fn_decl.name;
    signature->return_type = resolved_return;
    signature->param_types = NULL;
    signature->param_names = NULL;
    signature->param_count = BUF_LEN(decl->fn_decl.params);
    signature->is_pub = decl->fn_decl.is_pub;
    for (int32_t j = 0; j < signature->param_count; j++) {
        ASTNode *param = decl->fn_decl.params[j];
        const Type *param_type = resolve_ast_type(analyzer, &param->param.type);
        if (param_type == NULL) {
            param_type = &TYPE_ERR_INST;
        }
        BUF_PUSH(signature->param_types, param_type);
        BUF_PUSH(signature->param_names, param->param.name);
    }
    hash_table_insert(&analyzer->fn_table, decl->fn_decl.name, signature);

    scope_define(analyzer,
                 &(SymDef){decl->fn_decl.name, resolved_return, decl->fn_decl.is_pub, SYM_FN});
}

/**
 * Collect fields for a struct: promoted fields from embedded structs first,
 * then the struct's own fields (with duplicate checking).
 */
static void compose_struct_fields(Sema *analyzer, const ASTNode *decl, StructDef *def) {
    // Promote fields from embedded structs
    for (int32_t i = 0; i < BUF_LEN(def->embedded); i++) {
        StructDef *embed_def = sema_lookup_struct(analyzer, def->embedded[i]);
        if (embed_def == NULL) {
            SEMA_ERR(analyzer, decl->loc, "unknown embedded struct '%s'", def->embedded[i]);
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
                SEMA_ERR(analyzer, decl->loc, "duplicate field '%s'", ast_field->name);
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            const Type *field_type = resolve_ast_type(analyzer, &ast_field->type);
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
static void build_struct_type(Sema *analyzer, ASTNode *decl, StructDef *def) {
    const Type **embedded_types = NULL;
    StructField *type_fields = NULL;

    // Add embedded struct fields as named fields (e.g., "Base")
    for (int32_t i = 0; i < BUF_LEN(def->embedded); i++) {
        StructDef *embed_def = sema_lookup_struct(analyzer, def->embedded[i]);
        if (embed_def != NULL) {
            BUF_PUSH(embedded_types, embed_def->type);
            StructField sf = {.name = def->embedded[i], .type = embed_def->type};
            BUF_PUSH(type_fields, sf);
        }
    }

    // Add own fields
    for (int32_t i = 0; i < BUF_LEN(decl->struct_decl.fields); i++) {
        ASTStructField *ast_field = &decl->struct_decl.fields[i];
        const Type *field_type = resolve_ast_type(analyzer, &ast_field->type);
        if (field_type == NULL) {
            field_type = &TYPE_ERR_INST;
        }
        StructField sf = {.name = ast_field->name, .type = field_type};
        BUF_PUSH(type_fields, sf);
    }

    def->type = type_create_struct(analyzer->arena, def->name, type_fields, BUF_LEN(type_fields),
                                   embedded_types, BUF_LEN(embedded_types));
    decl->type = def->type;
}

/**
 * Register a method's signature in the fn table (keyed as "TypeName.method_name")
 * and append a StructMethodInfo entry to the methods buf.
 */
static void register_method_signature(Sema *analyzer, const char *type_name, ASTNode *method,
                                      StructMethodInfo **methods) {
    StructMethodInfo mi = {.name = method->fn_decl.name,
                           .is_mut_recv = method->fn_decl.is_mut_recv,
                           .is_ptr_recv = method->fn_decl.is_ptr_recv,
                           .recv_name = method->fn_decl.recv_name,
                           .decl = method};
    BUF_PUSH(*methods, mi);

    const char *method_key = arena_sprintf(analyzer->arena, "%s.%s", type_name, mi.name);

    const Type *return_type = &TYPE_UNIT_INST;
    if (method->fn_decl.return_type.kind != AST_TYPE_INFERRED) {
        return_type = resolve_ast_type(analyzer, &method->fn_decl.return_type);
        if (return_type == NULL) {
            return_type = &TYPE_UNIT_INST;
        }
    }

    FnSignature *sig = rsg_malloc(sizeof(*sig));
    sig->name = mi.name;
    sig->return_type = return_type;
    sig->param_types = NULL;
    sig->param_names = NULL;
    sig->param_count = BUF_LEN(method->fn_decl.params);
    sig->is_pub = false;

    for (int32_t j = 0; j < sig->param_count; j++) {
        ASTNode *param = method->fn_decl.params[j];
        const Type *pt = resolve_ast_type(analyzer, &param->param.type);
        if (pt == NULL) {
            pt = &TYPE_ERR_INST;
        }
        BUF_PUSH(sig->param_types, pt);
        BUF_PUSH(sig->param_names, param->param.name);
    }

    hash_table_insert(&analyzer->fn_table, method_key, sig);
}

/**
 * Register each struct method as a fn signature in the analyzer's
 * fn table (keyed as "StructName.method_name").
 */
static void register_struct_methods(Sema *analyzer, const ASTNode *decl, StructDef *def) {
    for (int32_t i = 0; i < BUF_LEN(decl->struct_decl.methods); i++) {
        register_method_signature(analyzer, def->name, decl->struct_decl.methods[i], &def->methods);
    }
}

/** Register a struct def: build the TYPE_STRUCT, register methods as fns. */
static void register_struct_def(Sema *analyzer, ASTNode *decl) {
    const char *struct_name = decl->struct_decl.name;

    // Check for duplicate struct def
    if (sema_lookup_struct(analyzer, struct_name) != NULL) {
        SEMA_ERR(analyzer, decl->loc, "duplicate struct def '%s'", struct_name);
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

    compose_struct_fields(analyzer, decl, def);
    build_struct_type(analyzer, decl, def);
    register_struct_methods(analyzer, decl, def);

    hash_table_insert(&analyzer->struct_table, struct_name, def);

    // Register struct as a type alias so resolve_ast_type can find it
    hash_table_insert(&analyzer->type_alias_table, struct_name, (void *)def->type);

    // Register struct name as a type sym
    scope_define(analyzer, &(SymDef){struct_name, def->type, false, SYM_TYPE});
}

/** Register an enum def: build the TYPE_ENUM, register methods as fns. */
/** Build a single EnumVariant from its AST representation. */
static EnumVariant build_enum_variant(Sema *analyzer, ASTEnumVariant *ast_variant,
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
            analyzer->arena, variant.tuple_count * sizeof(const Type *));
        for (int32_t j = 0; j < variant.tuple_count; j++) {
            variant.tuple_types[j] = resolve_ast_type(analyzer, &ast_variant->tuple_types[j]);
            if (variant.tuple_types[j] == NULL) {
                variant.tuple_types[j] = &TYPE_ERR_INST;
            }
        }
        break;
    }
    case VARIANT_STRUCT: {
        variant.kind = ENUM_VARIANT_STRUCT;
        variant.field_count = BUF_LEN(ast_variant->fields);
        variant.fields =
            arena_alloc_zero(analyzer->arena, variant.field_count * sizeof(StructField));
        for (int32_t j = 0; j < variant.field_count; j++) {
            variant.fields[j].name = ast_variant->fields[j].name;
            variant.fields[j].type = resolve_ast_type(analyzer, &ast_variant->fields[j].type);
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

static void register_enum_def(Sema *analyzer, ASTNode *decl) {
    const char *enum_name = decl->enum_decl.name;

    if (sema_lookup_enum(analyzer, enum_name) != NULL) {
        SEMA_ERR(analyzer, decl->loc, "duplicate enum def '%s'", enum_name);
        return;
    }

    EnumVariant *variants = NULL;
    int32_t auto_discriminant = 0;
    for (int32_t i = 0; i < BUF_LEN(decl->enum_decl.variants); i++) {
        EnumVariant variant =
            build_enum_variant(analyzer, &decl->enum_decl.variants[i], &auto_discriminant);
        BUF_PUSH(variants, variant);
    }

    const Type *enum_type =
        type_create_enum(analyzer->arena, enum_name, variants, BUF_LEN(variants));
    decl->type = enum_type;

    EnumDef *def = rsg_malloc(sizeof(*def));
    def->name = enum_name;
    def->methods = NULL;
    def->type = enum_type;

    for (int32_t i = 0; i < BUF_LEN(decl->enum_decl.methods); i++) {
        register_method_signature(analyzer, enum_name, decl->enum_decl.methods[i], &def->methods);
    }

    hash_table_insert(&analyzer->enum_table, enum_name, def);
    hash_table_insert(&analyzer->type_alias_table, enum_name, (void *)enum_type);
    scope_define(analyzer, &(SymDef){enum_name, enum_type, false, SYM_TYPE});
}

/**
 * Collect all required fields from a pact and its super pacts (recursively).
 * Appends to @p fields buf.
 */
static void collect_pact_fields(Sema *analyzer, const PactDef *pact, StructFieldInfo **fields,
                                SourceLoc loc) {
    // Recurse into super pacts
    for (int32_t i = 0; i < BUF_LEN(pact->super_pacts); i++) {
        PactDef *super = sema_lookup_pact(analyzer, pact->super_pacts[i]);
        if (super != NULL) {
            collect_pact_fields(analyzer, super, fields, loc);
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
static void collect_pact_methods(Sema *analyzer, const PactDef *pact, StructMethodInfo **methods,
                                 SourceLoc loc) {
    for (int32_t i = 0; i < BUF_LEN(pact->super_pacts); i++) {
        PactDef *super = sema_lookup_pact(analyzer, pact->super_pacts[i]);
        if (super != NULL) {
            collect_pact_methods(analyzer, super, methods, loc);
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

/** Register a pact def during pass 1. */
static void register_pact_def(Sema *analyzer, ASTNode *decl) {
    const char *pact_name = decl->pact_decl.name;

    if (sema_lookup_pact(analyzer, pact_name) != NULL) {
        SEMA_ERR(analyzer, decl->loc, "duplicate pact def '%s'", pact_name);
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
        const Type *field_type = resolve_ast_type(analyzer, &ast_field->type);
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

    hash_table_insert(&analyzer->pact_table, pact_name, def);
}

/**
 * Validate that a struct satisfies all pact conformances.
 * Checks required fields and methods exist, and injects default methods.
 */
static void validate_struct_conformances(Sema *analyzer, ASTNode *decl, StructDef *def) {
    for (int32_t ci = 0; ci < BUF_LEN(decl->struct_decl.conformances); ci++) {
        const char *pact_name = decl->struct_decl.conformances[ci];
        PactDef *pact = sema_lookup_pact(analyzer, pact_name);
        if (pact == NULL) {
            SEMA_ERR(analyzer, decl->loc, "unknown pact '%s'", pact_name);
            continue;
        }

        // Collect all required fields from this pact and its super pacts
        StructFieldInfo *required_fields = NULL;
        collect_pact_fields(analyzer, pact, &required_fields, decl->loc);

        // Check required fields exist in struct
        for (int32_t i = 0; i < BUF_LEN(required_fields); i++) {
            bool found = false;
            for (int32_t j = 0; j < BUF_LEN(def->fields); j++) {
                if (strcmp(def->fields[j].name, required_fields[i].name) == 0) {
                    if (!type_equal(def->fields[j].type, required_fields[i].type)) {
                        SEMA_ERR(analyzer, decl->loc,
                                 "field '%s' has type '%s' but pact '%s' requires type '%s'",
                                 required_fields[i].name,
                                 type_name(analyzer->arena, def->fields[j].type), pact_name,
                                 type_name(analyzer->arena, required_fields[i].type));
                    }
                    found = true;
                    break;
                }
            }
            if (!found) {
                SEMA_ERR(analyzer, decl->loc, "missing required field '%s' from pact '%s'",
                         required_fields[i].name, pact_name);
            }
        }
        BUF_FREE(required_fields);

        // Collect all required methods from this pact and its super pacts
        StructMethodInfo *pact_methods = NULL;
        collect_pact_methods(analyzer, pact, &pact_methods, decl->loc);

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
                    register_method_signature(analyzer, def->name, method_ast, &def->methods);
                } else {
                    SEMA_ERR(analyzer, decl->loc, "missing required method '%s' from pact '%s'",
                             pact_methods[i].name, pact_name);
                }
            }
        }
        BUF_FREE(pact_methods);
    }
}

bool sema_check(Sema *analyzer, ASTNode *file) {
    // Reset tables for each compilation
    hash_table_destroy(&analyzer->fn_table);
    hash_table_init(&analyzer->fn_table, NULL);
    hash_table_destroy(&analyzer->type_alias_table);
    hash_table_init(&analyzer->type_alias_table, NULL);
    hash_table_destroy(&analyzer->struct_table);
    hash_table_init(&analyzer->struct_table, NULL);
    hash_table_destroy(&analyzer->enum_table);
    hash_table_init(&analyzer->enum_table, NULL);
    hash_table_destroy(&analyzer->pact_table);
    hash_table_init(&analyzer->pact_table, NULL);

    scope_push(analyzer, false); // global scope

    // First pass: register struct defs, type aliases, and fn signatures

    // Register pacts first (they must be available when validating struct conformances)
    for (int32_t i = 0; i < BUF_LEN(file->file.decls); i++) {
        ASTNode *decl = file->file.decls[i];
        if (decl->kind == NODE_PACT_DECL) {
            register_pact_def(analyzer, decl);
        }
    }

    // Register structs (they may be refd by type aliases and fns)
    for (int32_t i = 0; i < BUF_LEN(file->file.decls); i++) {
        ASTNode *decl = file->file.decls[i];
        if (decl->kind == NODE_STRUCT_DECL) {
            register_struct_def(analyzer, decl);
        }
    }

    // Validate pact conformances after all structs and pacts are registered
    for (int32_t i = 0; i < BUF_LEN(file->file.decls); i++) {
        ASTNode *decl = file->file.decls[i];
        if (decl->kind == NODE_STRUCT_DECL && BUF_LEN(decl->struct_decl.conformances) > 0) {
            StructDef *def = sema_lookup_struct(analyzer, decl->struct_decl.name);
            if (def != NULL) {
                validate_struct_conformances(analyzer, decl, def);
            }
        }
    }

    // Register enum defs
    for (int32_t i = 0; i < BUF_LEN(file->file.decls); i++) {
        ASTNode *decl = file->file.decls[i];
        if (decl->kind == NODE_ENUM_DECL) {
            register_enum_def(analyzer, decl);
        }
    }

    for (int32_t i = 0; i < BUF_LEN(file->file.decls); i++) {
        ASTNode *decl = file->file.decls[i];

        if (decl->kind == NODE_TYPE_ALIAS) {
            const Type *underlying = resolve_ast_type(analyzer, &decl->type_alias.alias_type);
            if (underlying != NULL) {
                hash_table_insert(&analyzer->type_alias_table, decl->type_alias.name,
                                  (void *)underlying);
            }
        }

        if (decl->kind == NODE_FN_DECL) {
            register_fn_signature(analyzer, decl);
        }
    }

    // Second pass: type-check everything
    check_node(analyzer, file);

    scope_pop(analyzer);
    return analyzer->err_count == 0;
}
