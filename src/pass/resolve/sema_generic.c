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
    // TODO: extend to support TYPE_ENUM once enums can conform to pacts
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
    // Dynamic buffer to handle deeply nested generic types
    int32_t cap = 512;
    char *buf = rsg_malloc(cap);
    int32_t len = snprintf(buf, cap, "%s__", base);
    for (int32_t i = 0; i < count; i++) {
        if (i > 0) {
            len += snprintf(buf + len, (size_t)(cap - len), "_");
        }
        const char *tname = type_name(sema->arena, type_args[i]);
        int32_t tname_len = (int32_t)strlen(tname);
        // Grow if needed: current pos + type name + separator + NUL
        if (len + tname_len + 2 >= cap) {
            cap = (len + tname_len + 2) * 2;
            buf = rsg_realloc(buf, cap);
        }
        len = append_mangled_type(buf, len, cap, tname);
    }
    const char *result = arena_sprintf(sema->arena, "%s", buf);
    free(buf);
    return result;
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
        const Type *old = hash_table_lookup(&sema->generics.type_params, params[i].name);
        BUF_PUSH(saved, old);
        hash_table_insert(&sema->generics.type_params, params[i].name, (void *)resolved[i]);
    }
    *out_saved = saved;
}

/** Pop type param substitutions, restoring any previous values. */
static void pop_type_params(Sema *sema, ASTTypeParam *params, int32_t count, const Type **saved) {
    for (int32_t i = 0; i < count; i++) {
        if (saved[i] != NULL) {
            hash_table_insert(&sema->generics.type_params, params[i].name, (void *)saved[i]);
        } else {
            hash_table_remove(&sema->generics.type_params, params[i].name);
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

/** Grouped params for instantiating generic ext methods. */
typedef struct {
    const char *orig_name;          // Original generic type name (e.g. "Pair").
    const char *mangled;            // Mangled concrete name (e.g. "Pair__i32_str").
    const Type **resolved_args;     // Resolved concrete type args.
    int32_t arg_count;              // Number of type args.
    const Type *owner_type;         // Concrete Type* for the instantiation.
    StructMethodInfo **out_methods; // Buf of StructMethodInfo entries to append to.
    ASTNode *synth;                 // Synthetic AST node for cloned method decls.
    bool is_enum;                   // True if target is an enum.
} GenericExtInstSpec;

/**
 * Instantiate generic ext methods for a just-instantiated struct or enum.
 *
 * Scans all generic ext templates for ones targeting @p spec->orig_name (the
 * unmangled generic type name) with matching type param count, then clones and
 * registers their methods under @p spec->mangled.  Method bodies are
 * type-checked via the method_checker callback.
 */
static void instantiate_generic_ext_methods(Sema *sema, const GenericExtInstSpec *spec) {
    for (int32_t ei = 0; ei < BUF_LEN(sema->generic_ext_defs); ei++) {
        GenericExtDef *gext = sema->generic_ext_defs[ei];
        if (strcmp(gext->target_name, spec->orig_name) != 0) {
            continue;
        }
        if (gext->base.type_param_count != spec->arg_count) {
            continue;
        }

        // Push ext type params mapped to the concrete args
        const Type **ext_saved = NULL;
        push_type_params(sema, gext->base.type_params, spec->resolved_args, spec->arg_count,
                         &ext_saved);

        // Phase 1: Register all ext method sigs
        ASTNode *ext_decl = gext->base.decl;
        for (int32_t mi = 0; mi < BUF_LEN(ext_decl->ext_decl.methods); mi++) {
            ASTNode *method = ext_decl->ext_decl.methods[mi];
            StructMethodInfo method_info = {.name = method->fn_decl.name,
                                            .is_mut_recv = method->fn_decl.is_mut_recv,
                                            .is_ptr_recv = method->fn_decl.is_ptr_recv,
                                            .recv_name = method->fn_decl.recv_name,
                                            .decl = method};
            BUF_PUSH(*spec->out_methods, method_info);
            const char *method_key =
                arena_sprintf(sema->arena, "%s.%s", spec->mangled, method_info.name);
            FnSig *sig = build_fn_sig(sema, method, false);
            hash_table_insert(&sema->fn_table, method_key, sig);
        }

        // Phase 2: Clone and type-check ext method bodies
        for (int32_t mi = 0; mi < BUF_LEN(ext_decl->ext_decl.methods); mi++) {
            ASTNode *method = ext_decl->ext_decl.methods[mi];
            ASTNode *mclone = ast_clone(sema->arena, method);
            mclone->fn_decl.owner_struct = spec->mangled;
            mclone->fn_decl.type_params = NULL;
            if (spec->is_enum) {
                BUF_PUSH(spec->synth->enum_decl.methods, mclone);
            } else {
                BUF_PUSH(spec->synth->struct_decl.methods, mclone);
            }
            if (sema->method_checker != NULL) {
                sema->method_checker(sema, mclone, spec->mangled, spec->owner_type);
            }
        }

        pop_type_params(sema, gext->base.type_params, spec->arg_count, ext_saved);
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
static const char *generic_inst_preamble(Sema *sema, const GenericDef *gdef,
                                         const GenericInstArgs *args, const Type ***out_resolved) {
    // Count minimum required args (params without defaults)
    int32_t min_required = 0;
    for (int32_t i = 0; i < gdef->type_param_count; i++) {
        if (gdef->type_params[i].default_type == NULL) {
            min_required = i + 1;
        }
    }

    if (args->type_arg_count < min_required || args->type_arg_count > gdef->type_param_count) {
        if (min_required == gdef->type_param_count) {
            SEMA_ERR(sema, args->loc,
                     "wrong number of type arguments for '%s': expected %d, got %d", gdef->name,
                     gdef->type_param_count, args->type_arg_count);
        } else {
            SEMA_ERR(sema, args->loc,
                     "wrong number of type arguments for '%s': expected %d to %d, got %d",
                     gdef->name, min_required, gdef->type_param_count, args->type_arg_count);
        }
        *out_resolved = NULL;
        return NULL;
    }

    // Resolve explicit type args
    const Type **resolved = resolve_type_args(sema, args->type_args, args->type_arg_count);

    // Fill in defaults for missing args
    for (int32_t i = args->type_arg_count; i < gdef->type_param_count; i++) {
        const Type *dt = resolve_ast_type(sema, gdef->type_params[i].default_type);
        if (dt == NULL) {
            dt = &TYPE_ERR_INST;
        }
        BUF_PUSH(resolved, dt);
    }

    const char *mangled = build_mangled_name(sema, gdef->name, resolved, gdef->type_param_count);

    *out_resolved = resolved;
    return mangled;
}

// ── Unified generic type instantiation ────────────────────────────

/** Callbacks for the type-specific parts of struct/enum instantiation. */
typedef struct GenericTypeInstSpec {
    bool is_enum;
    /** Create the forward-declared stub type and register it in the sema tables. */
    Type *(*create_stub)(Sema *sema, const char *mangled);
    /**
     * Resolve fields/variants into the stub. Called between push and pop.
     * Returns pointer to the def's methods buf for ext method appending.
     */
    StructMethodInfo **(*resolve_members)(Sema *sema, ASTNode *orig, Type *stub,
                                          const char *mangled);
    /** Build synthetic AST node for lowering.  Returns the synth node. */
    ASTNode *(*build_synth)(Sema *sema, ASTNode *orig, const char *mangled, Type *stub);
    /** Get the owner type for method checking (stub itself for struct, ptr for enum). */
    const Type *(*owner_type)(Sema *sema, Type *stub);
} GenericTypeInstSpec;

// ── Struct-specific callbacks ─────────────────────────────────────

static Type *struct_create_stub(Sema *sema, const char *mangled) {
    StructDef *def = rsg_malloc(sizeof(*def));
    def->name = mangled;
    def->is_tuple_struct = false;
    def->fields = NULL;
    def->methods = NULL;
    def->embedded = NULL;
    def->assoc_types = NULL;

    StructTypeSpec stub_spec = {
        .name = mangled, .fields = NULL, .field_count = 0, .embedded = NULL, .embed_count = 0};
    Type *fwd_type = type_create_struct(sema->arena, &stub_spec);
    def->type = fwd_type;
    hash_table_insert(&sema->struct_table, mangled, def);
    hash_table_insert(&sema->type_alias_table, mangled, (void *)fwd_type);
    return fwd_type;
}

static StructMethodInfo **struct_resolve_members(Sema *sema, ASTNode *orig, Type *stub,
                                                 const char *mangled) {
    StructDef *def = sema_lookup_struct(sema, mangled);
    def->is_tuple_struct = orig->struct_decl.is_tuple_struct;

    for (int32_t i = 0; i < BUF_LEN(orig->struct_decl.fields); i++) {
        ASTStructField *ast_field = &orig->struct_decl.fields[i];
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

    // Backpatch the forward-declared Type* with resolved fields
    StructField *type_fields = NULL;
    for (int32_t i = 0; i < BUF_LEN(def->fields); i++) {
        StructField sf = {.name = def->fields[i].name,
                          .type = def->fields[i].type,
                          .is_pub = def->fields[i].is_pub};
        BUF_PUSH(type_fields, sf);
    }
    stub->struct_type.fields = type_fields;
    stub->struct_type.field_count = BUF_LEN(type_fields);

    register_generic_methods(sema, orig->struct_decl.methods, &def->methods, mangled);
    return &def->methods;
}

static ASTNode *struct_build_synth(Sema *sema, ASTNode *orig, const char *mangled, Type *stub) {
    ASTNode *synth = ast_new(sema->arena, NODE_STRUCT_DECL, orig->loc);
    synth->struct_decl.name = mangled;
    synth->struct_decl.is_tuple_struct = orig->struct_decl.is_tuple_struct;
    synth->struct_decl.fields = NULL;
    synth->struct_decl.methods = NULL;
    synth->struct_decl.embedded = NULL;
    synth->struct_decl.conformances = NULL;
    synth->struct_decl.type_params = NULL;

    for (int32_t i = 0; i < BUF_LEN(orig->struct_decl.fields); i++) {
        ASTStructField sf = orig->struct_decl.fields[i];
        BUF_PUSH(synth->struct_decl.fields, sf);
    }
    synth->type = stub;

    for (int32_t i = 0; i < BUF_LEN(orig->struct_decl.methods); i++) {
        ASTNode *method = orig->struct_decl.methods[i];
        ASTNode *clone = ast_clone(sema->arena, method);
        clone->fn_decl.owner_struct = mangled;
        clone->fn_decl.type_params = NULL;
        BUF_PUSH(synth->struct_decl.methods, clone);
        if (sema->method_checker != NULL) {
            sema->method_checker(sema, clone, mangled, stub);
        }
    }
    return synth;
}

static const Type *struct_owner_type(Sema *sema, Type *stub) {
    (void)sema;
    return stub;
}

// ── Enum-specific callbacks ───────────────────────────────────────

static Type *enum_create_stub(Sema *sema, const char *mangled) {
    Type *fwd_enum = type_create_enum(sema->arena, mangled, NULL, 0);
    EnumDef *def = rsg_malloc(sizeof(*def));
    def->name = mangled;
    def->methods = NULL;
    def->assoc_types = NULL;
    def->type = fwd_enum;
    hash_table_insert(&sema->enum_table, mangled, def);
    hash_table_insert(&sema->type_alias_table, mangled, (void *)fwd_enum);
    return fwd_enum;
}

static StructMethodInfo **enum_resolve_members(Sema *sema, ASTNode *orig, Type *stub,
                                               const char *mangled) {
    EnumDef *def = sema_lookup_enum(sema, mangled);

    // Copy associated types from AST and register as type aliases
    for (int32_t i = 0; i < BUF_LEN(orig->enum_decl.assoc_types); i++) {
        BUF_PUSH(def->assoc_types, orig->enum_decl.assoc_types[i]);
        if (orig->enum_decl.assoc_types[i].concrete_type != NULL) {
            const Type *at_type =
                resolve_ast_type(sema, orig->enum_decl.assoc_types[i].concrete_type);
            if (at_type != NULL && at_type->kind != TYPE_ERR) {
                hash_table_insert(&sema->type_alias_table, orig->enum_decl.assoc_types[i].name,
                                  (void *)at_type);
            }
        }
    }

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

    stub->enum_type.variants = variants;
    stub->enum_type.variant_count = BUF_LEN(variants);

    register_generic_methods(sema, orig->enum_decl.methods, &def->methods, mangled);
    return &def->methods;
}

static ASTNode *enum_build_synth(Sema *sema, ASTNode *orig, const char *mangled, Type *stub) {
    ASTNode *synth = ast_new(sema->arena, NODE_ENUM_DECL, orig->loc);
    synth->enum_decl.name = mangled;
    synth->enum_decl.variants = NULL;
    synth->enum_decl.methods = NULL;
    synth->enum_decl.type_params = NULL;
    synth->enum_decl.assoc_types = NULL;

    for (int32_t i = 0; i < BUF_LEN(orig->enum_decl.variants); i++) {
        BUF_PUSH(synth->enum_decl.variants, orig->enum_decl.variants[i]);
    }
    synth->type = stub;

    const Type *ptr_type = type_create_ptr(sema->arena, stub, false);
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
    return synth;
}

static const Type *enum_owner_type(Sema *sema, Type *stub) {
    return type_create_ptr(sema->arena, stub, false);
}

// ── Shared instantiation scaffold ─────────────────────────────────

static const GenericTypeInstSpec STRUCT_INST_SPEC = {
    .is_enum = false,
    .create_stub = struct_create_stub,
    .resolve_members = struct_resolve_members,
    .build_synth = struct_build_synth,
    .owner_type = struct_owner_type,
};

static const GenericTypeInstSpec ENUM_INST_SPEC = {
    .is_enum = true,
    .create_stub = enum_create_stub,
    .resolve_members = enum_resolve_members,
    .build_synth = enum_build_synth,
    .owner_type = enum_owner_type,
};

/**
 * Unified scaffolding for generic struct/enum instantiation.
 *
 * Steps: preamble → cache check → push_type_params → create stub →
 * resolve members → build synth → ext methods → pop_type_params.
 */
static const char *instantiate_generic_type(Sema *sema, GenericDef *gdef,
                                            const GenericInstArgs *args,
                                            const GenericTypeInstSpec *spec) {
    const Type **resolved_args = NULL;
    const char *mangled = generic_inst_preamble(sema, gdef, args, &resolved_args);
    if (mangled == NULL) {
        return NULL;
    }

    // Cache check
    if (spec->is_enum) {
        if (sema_lookup_enum(sema, mangled) != NULL) {
            BUF_FREE(resolved_args);
            return mangled;
        }
    } else {
        if (sema_lookup_struct(sema, mangled) != NULL) {
            BUF_FREE(resolved_args);
            return mangled;
        }
    }

    const Type **saved = NULL;
    push_type_params(sema, gdef->type_params, resolved_args, gdef->type_param_count, &saved);

    ASTNode *orig = gdef->decl;
    Type *stub = spec->create_stub(sema, mangled);

    StructMethodInfo **methods_ptr = spec->resolve_members(sema, orig, stub, mangled);

    ASTNode *synth = spec->build_synth(sema, orig, mangled, stub);

    GenericExtInstSpec ext_spec = {.orig_name = gdef->name,
                                   .mangled = mangled,
                                   .resolved_args = resolved_args,
                                   .arg_count = gdef->type_param_count,
                                   .owner_type = spec->owner_type(sema, stub),
                                   .out_methods = methods_ptr,
                                   .synth = synth,
                                   .is_enum = spec->is_enum};
    instantiate_generic_ext_methods(sema, &ext_spec);

    BUF_PUSH(sema->synthetic_decls, synth);

    pop_type_params(sema, gdef->type_params, gdef->type_param_count, saved);

    BUF_FREE(resolved_args);
    return mangled;
}

const char *instantiate_generic_struct(Sema *sema, GenericStructDef *gdef,
                                       const GenericInstArgs *args) {
    return instantiate_generic_type(sema, gdef, args, &STRUCT_INST_SPEC);
}

const char *instantiate_generic_enum(Sema *sema, GenericEnumDef *gdef,
                                     const GenericInstArgs *args) {
    return instantiate_generic_type(sema, gdef, args, &ENUM_INST_SPEC);
}
