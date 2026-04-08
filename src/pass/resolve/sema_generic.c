#include "_sema.h"

/**
 * @file sema_generic.c
 * @brief Generic instantiation — shared by resolve, check, and mono passes.
 *
 * Contains pact bound checking, name mangling, and generic struct/enum
 * instantiation.  Method body checking is dispatched through the
 * sema->method_checker callback (set by the check pass, NULL during resolve).
 */

// ── Pact bound checking ───────────────────────────────────────────

/** Check if @p type satisfies the pact bound @p bound_name (recursively). */
bool type_satisfies_bound(Sema *sema, const Type *type, const char *bound_name) {
    PactDef *pact = sema_lookup_pact(sema, bound_name);
    if (pact == NULL) {
        return false;
    }
    if (type == NULL || type->kind != TYPE_STRUCT) {
        return false;
    }
    const char *struct_name = type->struct_type.name;
    StructDef *sdef = sema_lookup_struct(sema, struct_name);
    if (sdef == NULL) {
        return false;
    }
    // Check required fields
    for (int32_t i = 0; i < BUF_LEN(pact->fields); i++) {
        bool found = false;
        for (int32_t j = 0; j < BUF_LEN(sdef->fields); j++) {
            if (strcmp(sdef->fields[j].name, pact->fields[i].name) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    // Check required methods (non-default)
    for (int32_t i = 0; i < BUF_LEN(pact->methods); i++) {
        bool has_default =
            pact->methods[i].decl != NULL && pact->methods[i].decl->fn_decl.body != NULL;
        if (has_default) {
            continue;
        }
        bool found = false;
        for (int32_t j = 0; j < BUF_LEN(sdef->methods); j++) {
            if (strcmp(sdef->methods[j].name, pact->methods[i].name) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    // Recursively check super pact requirements
    for (int32_t i = 0; i < BUF_LEN(pact->super_pacts); i++) {
        if (!type_satisfies_bound(sema, type, pact->super_pacts[i])) {
            return false;
        }
    }
    return true;
}

// ── Name mangling ─────────────────────────────────────────────────

/** Append a C-identifier-safe form of @p tname to @p buf at @p len. */
static int32_t append_mangled_type(char *buf, int32_t len, int32_t cap, const char *tname) {
    for (const char *p = tname; *p != '\0' && len < cap - 1; p++) {
        switch (*p) {
        case '*':
            len += snprintf(buf + len, (size_t)(cap - len), "ptr_");
            break;
        case '[':
        case ']':
        case '(':
        case ')':
        case ',':
        case ' ':
            break; // skip
        default:
            buf[len++] = *p;
            break;
        }
    }
    buf[len] = '\0';
    return len;
}

/** Build a mangled name for a generic instantiation: "fn__type1_type2". */
const char *build_mangled_name(Sema *sema, const char *base, const Type **type_args,
                               int32_t count) {
    // Start with "base__"
    char buf[512];
    int32_t len = snprintf(buf, sizeof(buf), "%s__", base);
    for (int32_t i = 0; i < count; i++) {
        if (i > 0) {
            len += snprintf(buf + len, sizeof(buf) - (size_t)len, "_");
        }
        const char *tname = type_name(sema->arena, type_args[i]);
        len = append_mangled_type(buf, len, (int32_t)sizeof(buf), tname);
    }
    return arena_sprintf(sema->arena, "%s", buf);
}

// ── Generic instantiation helpers ─────────────────────────────────

/** Resolve AST type args into concrete Type pointers. Caller must BUF_FREE. */
static const Type **resolve_type_args(Sema *sema, ASTType *type_args, int32_t count) {
    const Type **resolved = NULL;
    for (int32_t i = 0; i < count; i++) {
        const Type *t = resolve_ast_type(sema, &type_args[i]);
        if (t == NULL) {
            t = &TYPE_ERR_INST;
        }
        BUF_PUSH(resolved, t);
    }
    return resolved;
}

/** Push type param substitutions into the sema type_param_table. */
static void push_type_params(Sema *sema, ASTTypeParam *params, const Type **resolved, int32_t count,
                             const Type ***out_saved) {
    const Type **saved = NULL;
    for (int32_t i = 0; i < count; i++) {
        const Type *old = hash_table_lookup(&sema->type_param_table, params[i].name);
        BUF_PUSH(saved, old);
        hash_table_insert(&sema->type_param_table, params[i].name, (void *)resolved[i]);
    }
    *out_saved = saved;
}

/** Pop type param substitutions, restoring any previous values. */
static void pop_type_params(Sema *sema, ASTTypeParam *params, int32_t count, const Type **saved) {
    for (int32_t i = 0; i < count; i++) {
        if (saved[i] != NULL) {
            hash_table_insert(&sema->type_param_table, params[i].name, (void *)saved[i]);
        } else {
            hash_table_remove(&sema->type_param_table, params[i].name);
        }
    }
    BUF_FREE(saved);
}

/** Register methods from orig_methods under the mangled type name. */
static void register_generic_methods(Sema *sema, ASTNode **orig_methods,
                                     StructMethodInfo **out_methods, const char *mangled) {
    for (int32_t i = 0; i < BUF_LEN(orig_methods); i++) {
        ASTNode *method = orig_methods[i];
        StructMethodInfo mi = {.name = method->fn_decl.name,
                               .is_mut_recv = method->fn_decl.is_mut_recv,
                               .is_ptr_recv = method->fn_decl.is_ptr_recv,
                               .recv_name = method->fn_decl.recv_name,
                               .decl = method};
        BUF_PUSH(*out_methods, mi);

        const char *method_key = arena_sprintf(sema->arena, "%s.%s", mangled, mi.name);
        FnSig *sig = build_fn_sig(sema, method, false);
        hash_table_insert(&sema->fn_table, method_key, sig);
    }
}

/**
 * Instantiate generic ext methods for a just-instantiated struct or enum.
 *
 * Scans all generic ext templates for ones targeting @p orig_name (the unmangled
 * generic type name) with matching type param count, then clones and registers
 * their methods under @p mangled.  Method bodies are type-checked via the
 * method_checker callback.
 *
 * @param sema          Semantic context.
 * @param orig_name     Original generic type name (e.g. "Pair").
 * @param mangled       Mangled name for the concrete instantiation (e.g. "Pair__i32_str").
 * @param resolved_args Resolved concrete type args for the instantiation.
 * @param arg_count     Number of type args.
 * @param owner_type    The concrete Type* for the instantiation (struct or ptr to enum).
 * @param out_methods   Buf of StructMethodInfo entries to append to.
 * @param synth         Synthetic AST node for appending cloned method decls.
 * @param is_enum       True if the target is an enum (methods appended to enum_decl.methods).
 */
static void instantiate_generic_ext_methods(Sema *sema, const char *orig_name, const char *mangled,
                                            const Type **resolved_args, int32_t arg_count,
                                            const Type *owner_type, StructMethodInfo **out_methods,
                                            ASTNode *synth, bool is_enum) {
    for (int32_t ei = 0; ei < BUF_LEN(sema->generic_ext_defs); ei++) {
        GenericExtDef *gext = sema->generic_ext_defs[ei];
        if (strcmp(gext->target_name, orig_name) != 0) {
            continue;
        }
        if (gext->type_param_count != arg_count) {
            continue;
        }

        // Push ext type params mapped to the concrete args
        const Type **ext_saved = NULL;
        push_type_params(sema, gext->type_params, resolved_args, arg_count, &ext_saved);

        // Phase 1: Register all ext method sigs
        ASTNode *ext_decl = gext->decl;
        for (int32_t mi = 0; mi < BUF_LEN(ext_decl->ext_decl.methods); mi++) {
            ASTNode *method = ext_decl->ext_decl.methods[mi];
            StructMethodInfo method_info = {.name = method->fn_decl.name,
                                            .is_mut_recv = method->fn_decl.is_mut_recv,
                                            .is_ptr_recv = method->fn_decl.is_ptr_recv,
                                            .recv_name = method->fn_decl.recv_name,
                                            .decl = method};
            BUF_PUSH(*out_methods, method_info);
            const char *method_key = arena_sprintf(sema->arena, "%s.%s", mangled, method_info.name);
            FnSig *sig = build_fn_sig(sema, method, false);
            hash_table_insert(&sema->fn_table, method_key, sig);
        }

        // Phase 2: Clone and type-check ext method bodies
        for (int32_t mi = 0; mi < BUF_LEN(ext_decl->ext_decl.methods); mi++) {
            ASTNode *method = ext_decl->ext_decl.methods[mi];
            ASTNode *mclone = ast_clone(sema->arena, method);
            mclone->fn_decl.owner_struct = mangled;
            mclone->fn_decl.type_params = NULL;
            if (is_enum) {
                BUF_PUSH(synth->enum_decl.methods, mclone);
            } else {
                BUF_PUSH(synth->struct_decl.methods, mclone);
            }
            if (sema->method_checker != NULL) {
                sema->method_checker(sema, mclone, mangled, owner_type);
            }
        }

        pop_type_params(sema, gext->type_params, arg_count, ext_saved);
    }
}

// ── Generic struct instantiation ──────────────────────────────────

/**
 * Validate type arg count, resolve args, mangle name, and push type params.
 * Returns the mangled name on success (caller must proceed with instantiation),
 * or NULL if an error occurred.  Sets @p *out_resolved to the resolved args buf
 * (caller must BUF_FREE) and @p *out_mangled to the mangled name.
 *
 * If the type was already instantiated (lookup hit), returns the mangled name
 * and sets @p *out_resolved to NULL — caller should skip body instantiation.
 */
static const char *generic_inst_preamble(Sema *sema, const char *name, int32_t type_param_count,
                                         ASTTypeParam *type_params, const GenericInstArgs *args,
                                         const Type ***out_resolved) {
    // Count minimum required args (params without defaults)
    int32_t min_required = 0;
    for (int32_t i = 0; i < type_param_count; i++) {
        if (type_params[i].default_type == NULL) {
            min_required = i + 1;
        }
    }

    if (args->type_arg_count < min_required || args->type_arg_count > type_param_count) {
        if (min_required == type_param_count) {
            SEMA_ERR(sema, args->loc,
                     "wrong number of type arguments for '%s': expected %d, got %d", name,
                     type_param_count, args->type_arg_count);
        } else {
            SEMA_ERR(sema, args->loc,
                     "wrong number of type arguments for '%s': expected %d to %d, got %d", name,
                     min_required, type_param_count, args->type_arg_count);
        }
        *out_resolved = NULL;
        return NULL;
    }

    // Resolve explicit type args
    const Type **resolved = resolve_type_args(sema, args->type_args, args->type_arg_count);

    // Fill in defaults for missing args
    for (int32_t i = args->type_arg_count; i < type_param_count; i++) {
        const Type *dt = resolve_ast_type(sema, type_params[i].default_type);
        if (dt == NULL) {
            dt = &TYPE_ERR_INST;
        }
        BUF_PUSH(resolved, dt);
    }

    const char *mangled = build_mangled_name(sema, name, resolved, type_param_count);

    *out_resolved = resolved;
    return mangled;
}

const char *instantiate_generic_struct(Sema *sema, GenericStructDef *gdef,
                                       const GenericInstArgs *args) {
    const Type **resolved_args = NULL;
    const char *mangled =
        generic_inst_preamble(sema, gdef->name, gdef->type_param_count,
                              gdef->decl->struct_decl.type_params, args, &resolved_args);
    if (mangled == NULL) {
        return NULL;
    }
    if (sema_lookup_struct(sema, mangled) != NULL) {
        BUF_FREE(resolved_args);
        return mangled;
    }

    const Type **struct_saved = NULL;
    push_type_params(sema, gdef->type_params, resolved_args, gdef->type_param_count, &struct_saved);

    // Build concrete struct field types
    ASTNode *orig = gdef->decl;
    StructDef *def = rsg_malloc(sizeof(*def));
    def->name = mangled;
    def->fields = NULL;
    def->methods = NULL;
    def->embedded = NULL;

    // Forward-declare: register a stub Type* before resolving fields so that
    // recursive references (e.g., Node<T> containing ?*Node<T>) find the entry
    // in the cache and don't infinitely recurse.
    StructTypeSpec stub_spec = {
        .name = mangled, .fields = NULL, .field_count = 0, .embedded = NULL, .embed_count = 0};
    Type *fwd_type = type_create_struct(sema->arena, &stub_spec);
    def->type = fwd_type;
    hash_table_insert(&sema->struct_table, mangled, def);
    hash_table_insert(&sema->type_alias_table, mangled, (void *)fwd_type);

    for (int32_t i = 0; i < BUF_LEN(orig->struct_decl.fields); i++) {
        ASTStructField *ast_field = &orig->struct_decl.fields[i];
        const Type *field_type = resolve_ast_type(sema, &ast_field->type);
        if (field_type == NULL) {
            field_type = &TYPE_ERR_INST;
        }
        StructFieldInfo fi = {
            .name = ast_field->name, .type = field_type, .default_value = ast_field->default_value};
        BUF_PUSH(def->fields, fi);
    }

    // Backpatch the forward-declared Type* with resolved fields
    StructField *type_fields = NULL;
    for (int32_t i = 0; i < BUF_LEN(def->fields); i++) {
        StructField sf = {.name = def->fields[i].name, .type = def->fields[i].type};
        BUF_PUSH(type_fields, sf);
    }
    fwd_type->struct_type.fields = type_fields;
    fwd_type->struct_type.field_count = BUF_LEN(type_fields);

    register_generic_methods(sema, orig->struct_decl.methods, &def->methods, mangled);

    // Create synthetic AST node for lowering/codegen
    ASTNode *synth = ast_new(sema->arena, NODE_STRUCT_DECL, orig->loc);
    synth->struct_decl.name = mangled;
    synth->struct_decl.fields = NULL;
    synth->struct_decl.methods = NULL;
    synth->struct_decl.embedded = NULL;
    synth->struct_decl.conformances = NULL;
    synth->struct_decl.type_params = NULL; // concrete — no type params

    // Copy fields with resolved types
    for (int32_t i = 0; i < BUF_LEN(orig->struct_decl.fields); i++) {
        ASTStructField sf = orig->struct_decl.fields[i];
        BUF_PUSH(synth->struct_decl.fields, sf);
    }
    synth->type = def->type;

    // Clone and type-check method bodies (via callback if available)
    for (int32_t i = 0; i < BUF_LEN(orig->struct_decl.methods); i++) {
        ASTNode *method = orig->struct_decl.methods[i];
        ASTNode *clone = ast_clone(sema->arena, method);
        clone->fn_decl.owner_struct = mangled;
        clone->fn_decl.type_params = NULL;
        BUF_PUSH(synth->struct_decl.methods, clone);
        if (sema->method_checker != NULL) {
            sema->method_checker(sema, clone, mangled, def->type);
        }
    }

    // Instantiate generic ext methods (ext<T> Wrapper<T> { ... })
    instantiate_generic_ext_methods(sema, gdef->name, mangled, resolved_args,
                                    gdef->type_param_count, def->type, &def->methods, synth, false);

    // Append to file decls for lowering
    BUF_PUSH(sema->synthetic_decls, synth);

    pop_type_params(sema, gdef->type_params, gdef->type_param_count, struct_saved);

    BUF_FREE(resolved_args);
    return mangled;
}

// ── Generic enum instantiation ────────────────────────────────────

const char *instantiate_generic_enum(Sema *sema, GenericEnumDef *gdef,
                                     const GenericInstArgs *args) {
    const Type **resolved_args = NULL;
    const char *mangled =
        generic_inst_preamble(sema, gdef->name, gdef->type_param_count,
                              gdef->decl->enum_decl.type_params, args, &resolved_args);
    if (mangled == NULL) {
        return NULL;
    }
    if (sema_lookup_enum(sema, mangled) != NULL) {
        BUF_FREE(resolved_args);
        return mangled;
    }

    const Type **enum_saved = NULL;
    push_type_params(sema, gdef->type_params, resolved_args, gdef->type_param_count, &enum_saved);

    // Forward-declare: register a stub Type* before resolving variants so that
    // recursive references through option/result types don't infinitely recurse.
    ASTNode *orig = gdef->decl;
    Type *fwd_enum = type_create_enum(sema->arena, mangled, NULL, 0);
    EnumDef *def = rsg_malloc(sizeof(*def));
    def->name = mangled;
    def->methods = NULL;
    def->type = fwd_enum;
    hash_table_insert(&sema->enum_table, mangled, def);
    hash_table_insert(&sema->type_alias_table, mangled, (void *)fwd_enum);

    // Build concrete enum variants
    EnumVariant *variants = NULL;
    int32_t auto_discriminant = 0;
    for (int32_t i = 0; i < BUF_LEN(orig->enum_decl.variants); i++) {
        ASTEnumVariant *av = &orig->enum_decl.variants[i];
        EnumVariant variant = {0};
        variant.name = av->name;

        switch (av->kind) {
        case VARIANT_UNIT:
            variant.kind = ENUM_VARIANT_UNIT;
            break;
        case VARIANT_TUPLE: {
            variant.kind = ENUM_VARIANT_TUPLE;
            variant.tuple_count = BUF_LEN(av->tuple_types);
            variant.tuple_types = (const Type **)arena_alloc_zero(
                sema->arena, variant.tuple_count * sizeof(const Type *));
            for (int32_t j = 0; j < variant.tuple_count; j++) {
                variant.tuple_types[j] = resolve_ast_type(sema, &av->tuple_types[j]);
                if (variant.tuple_types[j] == NULL) {
                    variant.tuple_types[j] = &TYPE_ERR_INST;
                }
            }
            break;
        }
        case VARIANT_STRUCT: {
            variant.kind = ENUM_VARIANT_STRUCT;
            variant.field_count = BUF_LEN(av->fields);
            variant.fields =
                arena_alloc_zero(sema->arena, variant.field_count * sizeof(StructField));
            for (int32_t j = 0; j < variant.field_count; j++) {
                variant.fields[j].name = av->fields[j].name;
                variant.fields[j].type = resolve_ast_type(sema, &av->fields[j].type);
                if (variant.fields[j].type == NULL) {
                    variant.fields[j].type = &TYPE_ERR_INST;
                }
            }
            break;
        }
        }

        if (av->discriminant != NULL) {
            if (av->discriminant->kind == NODE_LIT && av->discriminant->lit.kind == LIT_I32) {
                variant.discriminant = (int32_t)av->discriminant->lit.integer_value;
                auto_discriminant = variant.discriminant + 1;
            }
        } else {
            variant.discriminant = auto_discriminant;
            auto_discriminant++;
        }
        BUF_PUSH(variants, variant);
    }

    // Backpatch the forward-declared enum Type* with resolved variants
    fwd_enum->enum_type.variants = variants;
    fwd_enum->enum_type.variant_count = BUF_LEN(variants);

    register_generic_methods(sema, orig->enum_decl.methods, &def->methods, mangled);

    // Create synthetic AST node for lowering/codegen
    ASTNode *synth = ast_new(sema->arena, NODE_ENUM_DECL, orig->loc);
    synth->enum_decl.name = mangled;
    synth->enum_decl.variants = NULL;
    synth->enum_decl.methods = NULL;
    synth->enum_decl.type_params = NULL; // concrete — no type params

    // Copy variants from original (they use AST types but lowering uses the Type*)
    for (int32_t i = 0; i < BUF_LEN(orig->enum_decl.variants); i++) {
        BUF_PUSH(synth->enum_decl.variants, orig->enum_decl.variants[i]);
    }
    synth->type = fwd_enum;

    // Clone and type-check method bodies (via callback if available)
    const Type *ptr_type = type_create_ptr(sema->arena, fwd_enum, false);
    for (int32_t i = 0; i < BUF_LEN(orig->enum_decl.methods); i++) {
        ASTNode *method = orig->enum_decl.methods[i];
        ASTNode *mclone = ast_clone(sema->arena, method);
        mclone->fn_decl.owner_struct = mangled;
        mclone->fn_decl.type_params = NULL;
        BUF_PUSH(synth->enum_decl.methods, mclone);
        if (sema->method_checker != NULL) {
            sema->method_checker(sema, mclone, mangled, ptr_type);
        }
    }

    // Instantiate generic ext methods (ext<L, R> Either<L, R> { ... })
    instantiate_generic_ext_methods(sema, gdef->name, mangled, resolved_args,
                                    gdef->type_param_count, ptr_type, &def->methods, synth, true);

    // Append to file decls for lowering
    BUF_PUSH(sema->synthetic_decls, synth);

    pop_type_params(sema, gdef->type_params, gdef->type_param_count, enum_saved);

    BUF_FREE(resolved_args);
    return mangled;
}
