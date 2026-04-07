#include "_check.h"

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

// ── Static helpers ─────────────────────────────────────────────────

/** Reset all hash tables and buffers for a fresh compilation. */
static void sema_reset_tables(Sema *sema) {
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
}

/**
 * First pass: register all declarations (pacts, builtins, structs,
 * conformances, enums, type aliases, fn sigs) into the sema tables.
 */
static void register_all_decls(Sema *sema, ASTNode *file) {
    // Register pacts first (they must be available when validating struct conformances)
    for (int32_t i = 0; i < BUF_LEN(file->file.decls); i++) {
        ASTNode *decl = file->file.decls[i];
        if (decl->kind == NODE_PACT_DECL) {
            register_pact_def(sema, decl);
        }
    }

    // Inject built-in generic enum templates: Option<T> and Result<T, E>
    // Must come before struct registration, since struct fields may use ?T / T!E.
    inject_builtin_enums(sema);

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

    // Register type aliases and fn sigs
    for (int32_t i = 0; i < BUF_LEN(file->file.decls); i++) {
        ASTNode *decl = file->file.decls[i];

        if (decl->kind == NODE_TYPE_ALIAS) {
            if (BUF_LEN(decl->type_alias.type_params) > 0) {
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
}

/** Clone, check, and emit a single pending generic fn instantiation. */
static void process_pending_generics(Sema *sema, ASTNode *file) {
    // Append synthetic decls from generic struct/enum instantiation
    for (int32_t i = 0; i < BUF_LEN(sema->synthetic_decls); i++) {
        BUF_PUSH(file->file.decls, sema->synthetic_decls[i]);
    }

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
        clone->fn_decl.body = ast_clone(sema->arena, orig->fn_decl.body);

        // Type-check the cloned fn body using the substitution context
        check_fn_body(sema, clone);

        // Append to file decls so lowering/codegen can see it
        BUF_PUSH(inst->file_node->file.decls, clone);

        // Clear type param substitutions
        hash_table_destroy(&sema->type_param_table);
        hash_table_init(&sema->type_param_table, NULL);
    }
}

// ── Public API ─────────────────────────────────────────────────────

bool sema_check(Sema *sema, ASTNode *file) {
    sema_reset_tables(sema);
    sema->file_node = file;

    scope_push(sema, false); // global scope

    register_all_decls(sema, file);
    check_node(sema, file);
    process_pending_generics(sema, file);

    scope_pop(sema);
    return sema->err_count == 0;
}
