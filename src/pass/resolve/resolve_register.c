#include "_sema.h"

/**
 * @file resolve_register.c
 * @brief Enum/pact registration and pact conformance enforcement.
 */

// ── Static helpers ─────────────────────────────────────────────────

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
            sema->base.arena, variant.tuple_count * sizeof(const Type *));
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
        variant.fields =
            arena_alloc_zero(sema->base.arena, variant.field_count * sizeof(StructField));
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
void collect_pact_fields(Sema *sema, const PactDef *pact, StructFieldInfo **fields) {
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
void collect_pact_methods(Sema *sema, const PactDef *pact, StructMethodInfo **methods) {
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

void register_enum_def(Sema *sema, ASTNode *decl) {
    const char *enum_name = decl->enum_decl->name;

    // If the enum has type params, store as a generic template instead
    if (BUF_LEN(decl->enum_decl->type_params) > 0) {
        GenericEnumDef *gdef = rsg_malloc(sizeof(*gdef));
        gdef->name = enum_name;
        gdef->decl = decl;
        gdef->type_params = decl->enum_decl->type_params;
        gdef->type_param_count = BUF_LEN(decl->enum_decl->type_params);
        hash_table_insert(&sema->base.generics.enums, enum_name, gdef);
        return;
    }

    if (sema_lookup_enum(sema, enum_name) != NULL) {
        SEMA_ERR(sema, decl->loc, "duplicate enum def '%s'", enum_name);
        return;
    }

    SEMA_INFER_SCOPE(sema, self_type_name, enum_name);

    // Register associated type aliases before variant resolution
    // so that variant types can reference them (e.g., Data(Payload))
    for (int32_t i = 0; i < BUF_LEN(decl->enum_decl->assoc_types); i++) {
        if (decl->enum_decl->assoc_types[i].concrete_type != NULL) {
            const Type *at_type =
                resolve_ast_type(sema, decl->enum_decl->assoc_types[i].concrete_type);
            if (at_type != NULL && at_type->kind != TYPE_ERR) {
                hash_table_insert(&sema->base.db.type_alias_table,
                                  decl->enum_decl->assoc_types[i].name, (void *)at_type);
            }
        }
    }

    EnumVariant *variants = NULL;
    int32_t auto_discriminant = 0;
    for (int32_t i = 0; i < BUF_LEN(decl->enum_decl->variants); i++) {
        EnumVariant variant =
            build_enum_variant(sema, &decl->enum_decl->variants[i], &auto_discriminant);
        BUF_PUSH(variants, variant);
    }

    const Type *enum_type =
        type_create_enum(sema->base.arena, enum_name, variants, BUF_LEN(variants));
    decl->type = enum_type;

    EnumDef *def = rsg_malloc(sizeof(*def));
    def->name = enum_name;
    def->methods = NULL;
    def->assoc_types = NULL;
    def->type = enum_type;

    // Copy associated types from AST
    for (int32_t i = 0; i < BUF_LEN(decl->enum_decl->assoc_types); i++) {
        BUF_PUSH(def->assoc_types, decl->enum_decl->assoc_types[i]);
    }

    // Register type alias early so Self-referencing method sigs can resolve
    hash_table_insert(&sema->base.db.enum_table, enum_name, def);
    hash_table_insert(&sema->base.db.type_alias_table, enum_name, (void *)enum_type);
    scope_define(sema, &(SymDef){enum_name, enum_type, false, SYM_TYPE});

    for (int32_t i = 0; i < BUF_LEN(decl->enum_decl->methods); i++) {
        register_method_sig(sema, enum_name, decl->enum_decl->methods[i], &def->methods);
    }

    SEMA_INFER_RESTORE(sema, self_type_name);
}

void register_pact_def(Sema *sema, ASTNode *decl) {
    const char *pact_name = decl->pact_decl->name;

    if (sema_lookup_pact(sema, pact_name) != NULL) {
        SEMA_ERR(sema, decl->loc, "duplicate pact def '%s'", pact_name);
        return;
    }

    SEMA_INFER_SCOPE(sema, self_type_name, pact_name);

    PactDef *def = rsg_malloc(sizeof(*def));
    def->name = pact_name;
    def->fields = NULL;
    def->methods = NULL;
    def->super_pacts = NULL;
    def->assoc_types = NULL;

    // Copy associated types from AST
    for (int32_t i = 0; i < BUF_LEN(decl->pact_decl->assoc_types); i++) {
        BUF_PUSH(def->assoc_types, decl->pact_decl->assoc_types[i]);
    }

    // Copy super pact refs
    for (int32_t i = 0; i < BUF_LEN(decl->pact_decl->super_pacts); i++) {
        BUF_PUSH(def->super_pacts, decl->pact_decl->super_pacts[i]);
    }

    // Register required fields
    for (int32_t i = 0; i < BUF_LEN(decl->pact_decl->fields); i++) {
        ASTStructField *ast_field = &decl->pact_decl->fields[i];
        const Type *field_type = resolve_ast_type(sema, &ast_field->type);
        if (field_type == NULL) {
            field_type = &TYPE_ERR_INST;
        }
        StructFieldInfo fi = {
            .name = ast_field->name, .type = field_type, .default_value = NULL, .is_pub = true};
        BUF_PUSH(def->fields, fi);
    }

    // Register methods
    for (int32_t i = 0; i < BUF_LEN(decl->pact_decl->methods); i++) {
        ASTNode *method = decl->pact_decl->methods[i];
        StructMethodInfo mi = {.name = method->fn_decl.name,
                               .is_mut_recv = method->fn_decl.is_mut_recv,
                               .is_ptr_recv = method->fn_decl.is_ptr_recv,
                               .recv_name = method->fn_decl.recv_name,
                               .decl = method};
        BUF_PUSH(def->methods, mi);
    }

    hash_table_insert(&sema->base.db.pact_table, pact_name, def);
    SEMA_INFER_RESTORE(sema, self_type_name);
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
                             required_fields[i].name,
                             type_name(sema->base.arena, def->fields[j].type), pact->name,
                             type_name(sema->base.arena, required_fields[i].type));
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
                BUF_PUSH(decl->struct_decl->methods, method_ast);
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
                BUF_PUSH(decl->struct_decl->assoc_types, defaulted);
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
void enforce_pact_assoc_type_bounds(Sema *sema, ASTNode *decl, StructDef *def,
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
                             type_name(sema->base.arena, concrete), bound);
                }
            }
            break;
        }
    }
}

void enforce_pact_conformances(Sema *sema, ASTNode *decl, StructDef *def) {
    for (int32_t ci = 0; ci < BUF_LEN(decl->struct_decl->conformances); ci++) {
        const char *pact_name = decl->struct_decl->conformances[ci];
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
