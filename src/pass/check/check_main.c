#include "_check.h"

// ── AST node deep clone (for generic monomorphization) ─────────────────

/** Recursively deep-clone an AST node and its children. */
ASTNode *clone_node(Arena *arena, ASTNode *src) {
    if (src == NULL) {
        return NULL;
    }
    ASTNode *dst = ast_new(arena, src->kind, src->loc);
    switch (src->kind) {
    case NODE_LIT:
        dst->lit = src->lit;
        break;
    case NODE_ID:
        dst->id = src->id;
        break;
    case NODE_UNARY:
        dst->unary.op = src->unary.op;
        dst->unary.operand = clone_node(arena, src->unary.operand);
        break;
    case NODE_BINARY:
        dst->binary.op = src->binary.op;
        dst->binary.left = clone_node(arena, src->binary.left);
        dst->binary.right = clone_node(arena, src->binary.right);
        break;
    case NODE_BLOCK:
        dst->block.stmts = NULL;
        for (int32_t i = 0; i < BUF_LEN(src->block.stmts); i++) {
            BUF_PUSH(dst->block.stmts, clone_node(arena, src->block.stmts[i]));
        }
        dst->block.result = clone_node(arena, src->block.result);
        break;
    case NODE_EXPR_STMT:
        dst->expr_stmt.expr = clone_node(arena, src->expr_stmt.expr);
        break;
    case NODE_VAR_DECL:
        dst->var_decl = src->var_decl;
        dst->var_decl.init = clone_node(arena, src->var_decl.init);
        break;
    case NODE_CALL:
        dst->call.callee = clone_node(arena, src->call.callee);
        dst->call.args = NULL;
        dst->call.arg_names = NULL;
        dst->call.arg_is_mut = NULL;
        dst->call.type_args = NULL;
        for (int32_t i = 0; i < BUF_LEN(src->call.args); i++) {
            BUF_PUSH(dst->call.args, clone_node(arena, src->call.args[i]));
        }
        for (int32_t i = 0; i < BUF_LEN(src->call.arg_names); i++) {
            BUF_PUSH(dst->call.arg_names, src->call.arg_names[i]);
        }
        for (int32_t i = 0; i < BUF_LEN(src->call.arg_is_mut); i++) {
            BUF_PUSH(dst->call.arg_is_mut, src->call.arg_is_mut[i]);
        }
        break;
    case NODE_MEMBER:
        dst->member.object = clone_node(arena, src->member.object);
        dst->member.member = src->member.member;
        break;
    case NODE_IDX:
        dst->idx_access.object = clone_node(arena, src->idx_access.object);
        dst->idx_access.idx = clone_node(arena, src->idx_access.idx);
        break;
    case NODE_IF:
        dst->if_expr.cond = clone_node(arena, src->if_expr.cond);
        dst->if_expr.then_body = clone_node(arena, src->if_expr.then_body);
        dst->if_expr.else_body = clone_node(arena, src->if_expr.else_body);
        break;
    case NODE_RETURN:
        dst->return_stmt.value = clone_node(arena, src->return_stmt.value);
        break;
    case NODE_ASSIGN:
        dst->assign.target = clone_node(arena, src->assign.target);
        dst->assign.value = clone_node(arena, src->assign.value);
        break;
    case NODE_COMPOUND_ASSIGN:
        dst->compound_assign.op = src->compound_assign.op;
        dst->compound_assign.target = clone_node(arena, src->compound_assign.target);
        dst->compound_assign.value = clone_node(arena, src->compound_assign.value);
        break;
    case NODE_STR_INTERPOLATION:
        dst->str_interpolation.parts = NULL;
        for (int32_t i = 0; i < BUF_LEN(src->str_interpolation.parts); i++) {
            BUF_PUSH(dst->str_interpolation.parts,
                     clone_node(arena, src->str_interpolation.parts[i]));
        }
        break;
    case NODE_TUPLE_LIT:
        dst->tuple_lit.elems = NULL;
        for (int32_t i = 0; i < BUF_LEN(src->tuple_lit.elems); i++) {
            BUF_PUSH(dst->tuple_lit.elems, clone_node(arena, src->tuple_lit.elems[i]));
        }
        break;
    case NODE_ARRAY_LIT:
        dst->array_lit = src->array_lit;
        dst->array_lit.elems = NULL;
        for (int32_t i = 0; i < BUF_LEN(src->array_lit.elems); i++) {
            BUF_PUSH(dst->array_lit.elems, clone_node(arena, src->array_lit.elems[i]));
        }
        break;
    case NODE_STRUCT_LIT:
        dst->struct_lit.name = src->struct_lit.name;
        dst->struct_lit.field_names = NULL;
        dst->struct_lit.field_values = NULL;
        for (int32_t i = 0; i < BUF_LEN(src->struct_lit.field_names); i++) {
            BUF_PUSH(dst->struct_lit.field_names, src->struct_lit.field_names[i]);
        }
        for (int32_t i = 0; i < BUF_LEN(src->struct_lit.field_values); i++) {
            BUF_PUSH(dst->struct_lit.field_values,
                     clone_node(arena, src->struct_lit.field_values[i]));
        }
        break;
    case NODE_ADDRESS_OF:
        dst->address_of.operand = clone_node(arena, src->address_of.operand);
        break;
    case NODE_DEREF:
        dst->deref.operand = clone_node(arena, src->deref.operand);
        break;
    case NODE_TYPE_CONVERSION:
        dst->type_conversion.target_type = src->type_conversion.target_type;
        dst->type_conversion.operand = clone_node(arena, src->type_conversion.operand);
        break;
    case NODE_LOOP:
        dst->loop.body = clone_node(arena, src->loop.body);
        break;
    case NODE_WHILE:
        dst->while_loop.cond = clone_node(arena, src->while_loop.cond);
        dst->while_loop.body = clone_node(arena, src->while_loop.body);
        break;
    case NODE_FOR:
        dst->for_loop = src->for_loop;
        dst->for_loop.start = clone_node(arena, src->for_loop.start);
        dst->for_loop.end = clone_node(arena, src->for_loop.end);
        dst->for_loop.iterable = clone_node(arena, src->for_loop.iterable);
        dst->for_loop.body = clone_node(arena, src->for_loop.body);
        break;
    case NODE_BREAK:
        dst->break_stmt.value = clone_node(arena, src->break_stmt.value);
        break;
    case NODE_CONTINUE:
        break;
    case NODE_DEFER:
        dst->defer_stmt.body = clone_node(arena, src->defer_stmt.body);
        break;
    case NODE_PARAM:
        dst->param = src->param;
        break;
    default:
        // Shallow copy for any unhandled kinds
        *dst = *src;
        dst->type = NULL;
        break;
    }
    return dst;
}

// ── Public API ─────────────────────────────────────────────────────────

Sema *sema_create(Arena *arena) {
    Sema *sema = rsg_malloc(sizeof(*sema));
    sema->arena = arena;
    sema->current_scope = NULL;
    sema->err_count = 0;
    sema->loop_break_type = NULL;
    sema->expected_type = NULL;
    sema->fn_return_type = NULL;
    sema->file_node = NULL;
    hash_table_init(&sema->type_alias_table, NULL);
    hash_table_init(&sema->fn_table, NULL);
    hash_table_init(&sema->struct_table, NULL);
    hash_table_init(&sema->enum_table, NULL);
    hash_table_init(&sema->pact_table, NULL);
    hash_table_init(&sema->generic_fn_table, NULL);
    hash_table_init(&sema->generic_struct_table, NULL);
    hash_table_init(&sema->generic_enum_table, NULL);
    hash_table_init(&sema->generic_type_alias_table, NULL);
    hash_table_init(&sema->type_param_table, NULL);
    sema->pending_insts = NULL;
    sema->synthetic_decls = NULL;
    return sema;
}

void sema_destroy(Sema *sema) {
    if (sema != NULL) {
        hash_table_destroy(&sema->type_alias_table);
        hash_table_destroy(&sema->fn_table);
        hash_table_destroy(&sema->struct_table);
        hash_table_destroy(&sema->enum_table);
        hash_table_destroy(&sema->pact_table);
        hash_table_destroy(&sema->generic_fn_table);
        hash_table_destroy(&sema->generic_struct_table);
        hash_table_destroy(&sema->generic_enum_table);
        hash_table_destroy(&sema->generic_type_alias_table);
        hash_table_destroy(&sema->type_param_table);
        BUF_FREE(sema->pending_insts);
        BUF_FREE(sema->synthetic_decls);
        free(sema);
    }
}

/** Register a single fn's sig into the sema tables and scope. */
static void register_fn_sig(Sema *sema, ASTNode *decl) {
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

/** Register a struct def: build the TYPE_STRUCT, register methods as fns. */
static void register_struct_def(Sema *sema, ASTNode *decl) {
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

/** Register an enum def: build the TYPE_ENUM, register methods as fns. */
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

static void register_enum_def(Sema *sema, ASTNode *decl) {
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

/**
 * Collect all required fields from a pact and its super pacts (recursively).
 * Appends to @p fields buf.
 */
static void collect_pact_fields(Sema *sema, const PactDef *pact, StructFieldInfo **fields,
                                SrcLoc loc) {
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

/** Register a pact def during pass 1. */
static void register_pact_def(Sema *sema, ASTNode *decl) {
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

/**
 * Validate that a struct satisfies all pact conformances.
 * Checks required fields and methods exist, and injects default methods.
 */
static void validate_struct_conformances(Sema *sema, ASTNode *decl, StructDef *def) {
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

bool sema_check(Sema *sema, ASTNode *file) {
    // Reset tables for each compilation
    hash_table_destroy(&sema->fn_table);
    hash_table_init(&sema->fn_table, NULL);
    hash_table_destroy(&sema->type_alias_table);
    hash_table_init(&sema->type_alias_table, NULL);
    hash_table_destroy(&sema->struct_table);
    hash_table_init(&sema->struct_table, NULL);
    hash_table_destroy(&sema->enum_table);
    hash_table_init(&sema->enum_table, NULL);
    hash_table_destroy(&sema->pact_table);
    hash_table_init(&sema->pact_table, NULL);
    hash_table_destroy(&sema->generic_fn_table);
    hash_table_init(&sema->generic_fn_table, NULL);
    hash_table_destroy(&sema->generic_struct_table);
    hash_table_init(&sema->generic_struct_table, NULL);
    hash_table_destroy(&sema->generic_enum_table);
    hash_table_init(&sema->generic_enum_table, NULL);
    hash_table_destroy(&sema->generic_type_alias_table);
    hash_table_init(&sema->generic_type_alias_table, NULL);
    hash_table_destroy(&sema->type_param_table);
    hash_table_init(&sema->type_param_table, NULL);
    BUF_FREE(sema->pending_insts);
    sema->pending_insts = NULL;
    BUF_FREE(sema->synthetic_decls);
    sema->synthetic_decls = NULL;
    sema->file_node = file;

    scope_push(sema, false); // global scope

    // First pass: register struct defs, type aliases, and fn sigs

    // Register pacts first (they must be available when validating struct conformances)
    for (int32_t i = 0; i < BUF_LEN(file->file.decls); i++) {
        ASTNode *decl = file->file.decls[i];
        if (decl->kind == NODE_PACT_DECL) {
            register_pact_def(sema, decl);
        }
    }

    // Inject built-in generic enum templates: Option<T> and Result<T, E>
    // Must come before struct registration, since struct fields may use ?T / T!E.
    {
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

    // Register structs (they may be refd by type aliases and fns)
    for (int32_t i = 0; i < BUF_LEN(file->file.decls); i++) {
        ASTNode *decl = file->file.decls[i];
        if (decl->kind == NODE_STRUCT_DECL) {
            register_struct_def(sema, decl);
        }
    }

    // Validate pact conformances after all structs and pacts are registered
    for (int32_t i = 0; i < BUF_LEN(file->file.decls); i++) {
        ASTNode *decl = file->file.decls[i];
        if (decl->kind == NODE_STRUCT_DECL && BUF_LEN(decl->struct_decl.conformances) > 0) {
            StructDef *def = sema_lookup_struct(sema, decl->struct_decl.name);
            if (def != NULL) {
                validate_struct_conformances(sema, decl, def);
            }
        }
    }

    // Register enum defs
    for (int32_t i = 0; i < BUF_LEN(file->file.decls); i++) {
        ASTNode *decl = file->file.decls[i];
        if (decl->kind == NODE_ENUM_DECL) {
            register_enum_def(sema, decl);
        }
    }

    for (int32_t i = 0; i < BUF_LEN(file->file.decls); i++) {
        ASTNode *decl = file->file.decls[i];

        if (decl->kind == NODE_TYPE_ALIAS) {
            if (BUF_LEN(decl->type_alias.type_params) > 0) {
                // Generic type alias: store as template
                GenericTypeAlias *gta = rsg_malloc(sizeof(*gta));
                gta->name = decl->type_alias.name;
                gta->alias_type = decl->type_alias.alias_type;
                gta->type_params = decl->type_alias.type_params;
                gta->type_param_count = BUF_LEN(decl->type_alias.type_params);
                hash_table_insert(&sema->generic_type_alias_table, gta->name, gta);
            } else {
                const Type *underlying = resolve_ast_type(sema, &decl->type_alias.alias_type);
                if (underlying != NULL) {
                    hash_table_insert(&sema->type_alias_table, decl->type_alias.name,
                                      (void *)underlying);
                }
            }
        }

        if (decl->kind == NODE_FN_DECL) {
            register_fn_sig(sema, decl);
        }
    }

    // Second pass: type-check everything
    check_node(sema, file);

    // Append synthetic decls from generic struct/enum instantiation
    for (int32_t i = 0; i < BUF_LEN(sema->synthetic_decls); i++) {
        BUF_PUSH(file->file.decls, sema->synthetic_decls[i]);
    }

    // Process pending generic instantiations (deferred body checking)
    for (int32_t gi = 0; gi < BUF_LEN(sema->pending_insts); gi++) {
        GenericInst *inst = &sema->pending_insts[gi];
        GenericFnDef *gdef = inst->generic;

        // Push type param substitutions
        for (int32_t ti = 0; ti < gdef->type_param_count; ti++) {
            hash_table_insert(&sema->type_param_table, gdef->type_params[ti].name,
                              (void *)inst->type_args[ti]);
        }

        // Create a cloned fn_decl with the mangled name and concrete types
        ASTNode *orig = gdef->decl;
        ASTNode *clone = ast_new(sema->arena, NODE_FN_DECL, orig->loc);
        clone->fn_decl.is_pub = false;
        clone->fn_decl.name = inst->mangled_name;
        clone->fn_decl.params = NULL;
        clone->fn_decl.type_params = NULL;
        clone->fn_decl.recv_name = NULL;
        clone->fn_decl.is_mut_recv = false;
        clone->fn_decl.is_ptr_recv = false;
        clone->fn_decl.owner_struct = NULL;

        // Clone params with substituted types
        for (int32_t pi = 0; pi < BUF_LEN(orig->fn_decl.params); pi++) {
            ASTNode *op = orig->fn_decl.params[pi];
            ASTNode *np = ast_new(sema->arena, NODE_PARAM, op->loc);
            np->param.name = op->param.name;
            np->param.type = op->param.type;
            np->param.is_mut = op->param.is_mut;
            BUF_PUSH(clone->fn_decl.params, np);
        }

        // Copy return type (resolve_ast_type will use type_param_table)
        clone->fn_decl.return_type = orig->fn_decl.return_type;
        clone->fn_decl.body = clone_node(sema->arena, orig->fn_decl.body);

        // Type-check the cloned fn body using the substitution context
        check_fn_body(sema, clone);

        // Append to file decls so lowering/codegen can see it
        BUF_PUSH(inst->file_node->file.decls, clone);

        // Clear type param substitutions
        hash_table_destroy(&sema->type_param_table);
        hash_table_init(&sema->type_param_table, NULL);
    }

    scope_pop(sema);
    return sema->err_count == 0;
}
