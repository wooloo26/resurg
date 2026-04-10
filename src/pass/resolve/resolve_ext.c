#include "_sema.h"

/**
 * @file resolve_ext.c
 * @brief Extension block registration and pact conformance enforcement for ext decls.
 */

// ── Static helpers ─────────────────────────────────────────────────

/** Push ext-declared associated types into @p target_buf and register as type aliases. */
static void register_ext_assoc_types(Sema *sema, const ASTNode *decl, ASTAssocType **target_buf) {
    for (int32_t i = 0; i < BUF_LEN(decl->ext_decl.assoc_types); i++) {
        BUF_PUSH(*target_buf, decl->ext_decl.assoc_types[i]);
        if (decl->ext_decl.assoc_types[i].concrete_type != NULL) {
            const Type *at_type =
                resolve_ast_type(sema, decl->ext_decl.assoc_types[i].concrete_type);
            if (at_type != NULL && at_type->kind != TYPE_ERR) {
                hash_table_insert(&sema->db.type_alias_table, decl->ext_decl.assoc_types[i].name,
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
    hash_table_insert(&sema->db.fn_table, method_key, sig);
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

// ── Public API ─────────────────────────────────────────────────────

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
        const char *prev_self = sema->infer.self_type_name;
        sema->infer.self_type_name = target_name;
        register_ext_assoc_types(sema, decl, &sdef->assoc_types);
        register_ext_methods(sema, target_name, decl, &sdef->methods);
        sema->infer.self_type_name = prev_self;
        return;
    }

    // Try to find the target as an enum
    EnumDef *edef = sema_lookup_enum(sema, target_name);
    if (edef != NULL) {
        const char *prev_self = sema->infer.self_type_name;
        sema->infer.self_type_name = target_name;
        register_ext_assoc_types(sema, decl, &edef->assoc_types);
        register_ext_methods(sema, target_name, decl, &edef->methods);
        sema->infer.self_type_name = prev_self;
        return;
    }

    // Primitive type
    for (int32_t i = 0; i < BUF_LEN(decl->ext_decl.methods); i++) {
        register_primitive_method(sema, target_name, decl->ext_decl.methods[i]);
    }
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
