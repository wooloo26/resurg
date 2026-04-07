#include "_check.h"

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

// ── Generic struct instantiation ──────────────────────────────────

/**
 * Instantiate a generic struct with the given type args.
 * Creates a concrete struct def with a mangled name and registers it.
 * Returns the mangled name, or NULL on error.
 */
const char *instantiate_generic_struct(Sema *sema, GenericStructDef *gdef, ASTType *type_args,
                                       int32_t type_arg_count, SrcLoc loc) {
    int32_t expected = gdef->type_param_count;
    if (type_arg_count != expected) {
        SEMA_ERR(sema, loc, "wrong number of type arguments for '%s': expected %d, got %d",
                 gdef->name, expected, type_arg_count);
        return NULL;
    }

    // Resolve type args
    const Type **resolved_args = NULL;
    for (int32_t i = 0; i < type_arg_count; i++) {
        const Type *t = resolve_ast_type(sema, &type_args[i]);
        if (t == NULL) {
            t = &TYPE_ERR_INST;
        }
        BUF_PUSH(resolved_args, t);
    }

    // Build mangled name
    const char *mangled = build_mangled_name(sema, gdef->name, resolved_args, type_arg_count);

    // Check if already instantiated
    if (sema_lookup_struct(sema, mangled) != NULL) {
        BUF_FREE(resolved_args);
        return mangled;
    }

    // Push type param substitutions
    for (int32_t i = 0; i < expected; i++) {
        hash_table_insert(&sema->type_param_table, gdef->type_params[i].name,
                          (void *)resolved_args[i]);
    }

    // Build concrete struct field types
    ASTNode *orig = gdef->decl;
    StructDef *def = rsg_malloc(sizeof(*def));
    def->name = mangled;
    def->fields = NULL;
    def->methods = NULL;
    def->embedded = NULL;

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

    // Build the Type*
    StructField *type_fields = NULL;
    for (int32_t i = 0; i < BUF_LEN(def->fields); i++) {
        StructField sf = {.name = def->fields[i].name, .type = def->fields[i].type};
        BUF_PUSH(type_fields, sf);
    }

    StructTypeSpec spec = {
        .name = mangled,
        .fields = type_fields,
        .field_count = BUF_LEN(type_fields),
        .embedded = NULL,
        .embed_count = 0,
    };
    def->type = type_create_struct(sema->arena, &spec);

    // Register methods with the mangled struct name
    for (int32_t i = 0; i < BUF_LEN(orig->struct_decl.methods); i++) {
        ASTNode *method = orig->struct_decl.methods[i];
        StructMethodInfo mi = {.name = method->fn_decl.name,
                               .is_mut_recv = method->fn_decl.is_mut_recv,
                               .is_ptr_recv = method->fn_decl.is_ptr_recv,
                               .recv_name = method->fn_decl.recv_name,
                               .decl = method};
        BUF_PUSH(def->methods, mi);

        const char *method_key = arena_sprintf(sema->arena, "%s.%s", mangled, mi.name);

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

    hash_table_insert(&sema->struct_table, mangled, def);
    hash_table_insert(&sema->type_alias_table, mangled, (void *)def->type);

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

    // Clone and type-check method bodies with type param substitutions
    for (int32_t i = 0; i < BUF_LEN(orig->struct_decl.methods); i++) {
        ASTNode *method = orig->struct_decl.methods[i];
        ASTNode *clone = ast_clone(sema->arena, method);
        clone->fn_decl.owner_struct = mangled;
        clone->fn_decl.type_params = NULL;
        BUF_PUSH(synth->struct_decl.methods, clone);
        check_struct_method_body(sema, clone, mangled, def->type);
    }

    // Append to file decls for lowering
    BUF_PUSH(sema->synthetic_decls, synth);

    // Clear type param substitutions
    for (int32_t i = 0; i < expected; i++) {
        hash_table_remove(&sema->type_param_table, gdef->type_params[i].name);
    }

    BUF_FREE(resolved_args);
    return mangled;
}

// ── Generic enum instantiation ────────────────────────────────────

/**
 * Instantiate a generic enum with the given type args.
 * Creates a concrete enum def with a mangled name and registers it.
 * Returns the mangled name, or NULL on error.
 */
const char *instantiate_generic_enum(Sema *sema, GenericEnumDef *gdef, ASTType *type_args,
                                     int32_t type_arg_count, SrcLoc loc) {
    int32_t expected = gdef->type_param_count;
    if (type_arg_count != expected) {
        SEMA_ERR(sema, loc, "wrong number of type arguments for '%s': expected %d, got %d",
                 gdef->name, expected, type_arg_count);
        return NULL;
    }

    // Resolve type args
    const Type **resolved_args = NULL;
    for (int32_t i = 0; i < type_arg_count; i++) {
        const Type *t = resolve_ast_type(sema, &type_args[i]);
        if (t == NULL) {
            t = &TYPE_ERR_INST;
        }
        BUF_PUSH(resolved_args, t);
    }

    // Build mangled name
    const char *mangled = build_mangled_name(sema, gdef->name, resolved_args, type_arg_count);

    // Check if already instantiated
    if (sema_lookup_enum(sema, mangled) != NULL) {
        BUF_FREE(resolved_args);
        return mangled;
    }

    // Push type param substitutions
    for (int32_t i = 0; i < expected; i++) {
        hash_table_insert(&sema->type_param_table, gdef->type_params[i].name,
                          (void *)resolved_args[i]);
    }

    // Build concrete enum variants
    ASTNode *orig = gdef->decl;
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

    const Type *enum_type = type_create_enum(sema->arena, mangled, variants, BUF_LEN(variants));

    EnumDef *def = rsg_malloc(sizeof(*def));
    def->name = mangled;
    def->methods = NULL;
    def->type = enum_type;

    // Register methods with the mangled enum name
    for (int32_t i = 0; i < BUF_LEN(orig->enum_decl.methods); i++) {
        ASTNode *method = orig->enum_decl.methods[i];
        StructMethodInfo mi = {.name = method->fn_decl.name,
                               .is_mut_recv = method->fn_decl.is_mut_recv,
                               .is_ptr_recv = method->fn_decl.is_ptr_recv,
                               .recv_name = method->fn_decl.recv_name,
                               .decl = method};
        BUF_PUSH(def->methods, mi);

        const char *method_key = arena_sprintf(sema->arena, "%s.%s", mangled, mi.name);

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

    hash_table_insert(&sema->enum_table, mangled, def);
    hash_table_insert(&sema->type_alias_table, mangled, (void *)enum_type);

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
    synth->type = enum_type;

    // Clone and type-check method bodies with type param substitutions
    const Type *ptr_type = type_create_ptr(sema->arena, enum_type, false);
    for (int32_t i = 0; i < BUF_LEN(orig->enum_decl.methods); i++) {
        ASTNode *method = orig->enum_decl.methods[i];
        ASTNode *mclone = ast_clone(sema->arena, method);
        mclone->fn_decl.owner_struct = mangled;
        mclone->fn_decl.type_params = NULL;
        BUF_PUSH(synth->enum_decl.methods, mclone);
        check_struct_method_body(sema, mclone, mangled, ptr_type);
    }

    // Append to file decls for lowering
    BUF_PUSH(sema->synthetic_decls, synth);

    // Clear type param substitutions
    for (int32_t i = 0; i < expected; i++) {
        hash_table_remove(&sema->type_param_table, gdef->type_params[i].name);
    }

    BUF_FREE(resolved_args);
    return mangled;
}
