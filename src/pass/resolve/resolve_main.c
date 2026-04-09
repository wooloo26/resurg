#include "_sema.h"
#include "rsg/pass/resolve/resolve.h"

/**
 * @file resolve_main.c
 * @brief Resolve pass — register declarations and populate symbol tables.
 *
 * First semantic pass: pushes the global scope, registers all pacts,
 * structs, enums, type aliases, and fn sigs into the Sema tables.
 */

// ── Static helpers ─────────────────────────────────────────────────

#ifdef _WIN32
#define PATH_SEP '\\'
#else
#define PATH_SEP '/'
#endif

/** Compute the directory portion of @p file_path (arena-allocated). */
static const char *dir_of(Arena *arena, const char *file_path) {
    const char *last_sep = strrchr(file_path, '/');
#ifdef _WIN32
    const char *last_bsep = strrchr(file_path, '\\');
    if (last_bsep != NULL && (last_sep == NULL || last_bsep > last_sep)) {
        last_sep = last_bsep;
    }
#endif
    if (last_sep == NULL) {
        return arena_strdup(arena, ".");
    }
    size_t len = (size_t)(last_sep - file_path);
    char *dir = arena_alloc(arena, len + 1);
    memcpy(dir, file_path, len);
    dir[len] = '\0';
    return dir;
}

/**
 * Load a filesystem module via the injected module loader callback.
 * Returns the parsed declarations or NULL on failure.
 */
ASTNode **load_module_decls(Sema *sema, const char *mod_path) {
    if (sema->module_loader == NULL) {
        return NULL;
    }
    return sema->module_loader(sema->module_loader_ctx, sema->arena, mod_path);
}

/** Destroy and reinitialize all hash tables and buffers for a fresh compilation. */
static void sema_destroy_and_reinit_tables(Sema *sema) {
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
    hash_table_destroy(&sema->generics.fn);
    hash_table_init(&sema->generics.fn, NULL);
    hash_table_destroy(&sema->generics.structs);
    hash_table_init(&sema->generics.structs, NULL);
    hash_table_destroy(&sema->generics.enums);
    hash_table_init(&sema->generics.enums, NULL);
    hash_table_destroy(&sema->generics.type_alias);
    hash_table_init(&sema->generics.type_alias, NULL);
    hash_table_destroy(&sema->generics.type_params);
    hash_table_init(&sema->generics.type_params, NULL);
    builtin_registry_destroy(&sema->builtins);
    builtin_registry_init(&sema->builtins);
    BUF_FREE(sema->pending_insts);
    sema->pending_insts = NULL;
    BUF_FREE(sema->generic_ext_defs);
    sema->generic_ext_defs = NULL;
    BUF_FREE(sema->synthetic_decls);
    sema->synthetic_decls = NULL;
}

/** Register a single type alias (concrete or generic). */
static void register_type_alias(Sema *sema, ASTNode *decl) {
    if (BUF_LEN(decl->type_alias.type_params) > 0) {
        GenericTypeAlias *gta = rsg_malloc(sizeof(*gta));
        gta->base.name = decl->type_alias.name;
        gta->base.decl = decl;
        gta->alias_type = decl->type_alias.alias_type;
        gta->base.type_params = decl->type_alias.type_params;
        gta->base.type_param_count = BUF_LEN(decl->type_alias.type_params);
        hash_table_insert(&sema->generics.type_alias, gta->base.name, gta);
    } else {
        const Type *underlying = resolve_ast_type(sema, &decl->type_alias.alias_type);
        if (underlying != NULL) {
            hash_table_insert(&sema->type_alias_table, decl->type_alias.name, (void *)underlying);
        }
    }
}

/**
 * Register all declarations (pacts, builtins, structs,
 * conformances, enums, type aliases, fn sigs) into the sema tables.
 */
static void register_all_decls(Sema *sema, ASTNode *file) {
    // Register nested modules first (their decls need to be available for other registrations)
    for (int32_t i = 0; i < BUF_LEN(file->file.decls); i++) {
        ASTNode *decl = file->file.decls[i];
        if (decl->kind != NODE_MODULE) {
            continue;
        }
        if (decl->module.decls == NULL) {
            // Filesystem module resolution: load <name>.rsg from module search dir
            const char *mod_path = arena_sprintf(sema->arena, "%s%c%s.rsg", sema->module_search_dir,
                                                 PATH_SEP, decl->module.name);
            ASTNode **decls = load_module_decls(sema, mod_path);
            if (decls == NULL) {
                SEMA_ERR(sema, decl->loc, "cannot find module file '%s'", mod_path);
                continue;
            }
            decl->module.decls = decls;

            // Child modules of this file search in <search_dir>/<name>/
            const char *prev_dir = sema->module_search_dir;
            sema->module_search_dir =
                arena_sprintf(sema->arena, "%s%c%s", prev_dir, PATH_SEP, decl->module.name);
            register_module_decl(sema, decl);
            sema->module_search_dir = prev_dir;
            continue;
        }
        register_module_decl(sema, decl);
    }

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
                enforce_pact_conformances(sema, decl, def);
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
            register_type_alias(sema, decl);
        }
        if (decl->kind == NODE_FN_DECL) {
            register_fn_sig(sema, decl);
        }
    }

    // Register extension methods and validate conformances
    for (int32_t i = 0; i < BUF_LEN(file->file.decls); i++) {
        ASTNode *decl = file->file.decls[i];
        if (decl->kind == NODE_EXT_DECL) {
            register_ext_decl(sema, decl);
        }
    }
    for (int32_t i = 0; i < BUF_LEN(file->file.decls); i++) {
        ASTNode *decl = file->file.decls[i];
        if (decl->kind == NODE_EXT_DECL && BUF_LEN(decl->ext_decl.impl_pacts) > 0) {
            enforce_ext_pact_conformances(sema, decl);
        }
    }

    // Resolve use imports (after all modules' decls are registered)
    for (int32_t i = 0; i < BUF_LEN(file->file.decls); i++) {
        ASTNode *decl = file->file.decls[i];
        if (decl->kind == NODE_USE_DECL) {
            register_use_decl(sema, decl);
        }
    }
}

// ── Public API ─────────────────────────────────────────────────────────

void sema_set_module_loader(Sema *sema, ModuleLoader loader, void *ctx) {
    sema->module_loader = loader;
    sema->module_loader_ctx = ctx;
}

Sema *sema_create(Arena *arena) {
    Sema *sema = rsg_malloc(sizeof(*sema));
    sema->arena = arena;
    sema->current_scope = NULL;
    sema->err_count = 0;
    sema->loop_break_type = NULL;
    sema->expected_type = NULL;
    sema->fn_return_type = NULL;
    sema->self_type_name = NULL;
    sema->current_module = NULL;
    sema->module_search_dir = NULL;
    sema->module_loader = NULL;
    sema->module_loader_ctx = NULL;
    sema->fn_body_checker = NULL;
    sema->closure = (ClosureCtx){0};
    sema->file_node = NULL;
    sema->method_checker = NULL;
    hash_table_init(&sema->type_alias_table, NULL);
    hash_table_init(&sema->fn_table, NULL);
    hash_table_init(&sema->struct_table, NULL);
    hash_table_init(&sema->enum_table, NULL);
    hash_table_init(&sema->pact_table, NULL);
    hash_table_init(&sema->generics.fn, NULL);
    hash_table_init(&sema->generics.structs, NULL);
    hash_table_init(&sema->generics.enums, NULL);
    hash_table_init(&sema->generics.type_alias, NULL);
    hash_table_init(&sema->generics.type_params, NULL);
    builtin_registry_init(&sema->builtins);
    sema->pending_insts = NULL;
    sema->generic_ext_defs = NULL;
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
        hash_table_destroy(&sema->generics.fn);
        hash_table_destroy(&sema->generics.structs);
        hash_table_destroy(&sema->generics.enums);
        hash_table_destroy(&sema->generics.type_alias);
        hash_table_destroy(&sema->generics.type_params);
        builtin_registry_destroy(&sema->builtins);
        BUF_FREE(sema->pending_insts);
        BUF_FREE(sema->generic_ext_defs);
        BUF_FREE(sema->synthetic_decls);
        free(sema);
    }
}

bool sema_resolve(Sema *sema, ASTNode *file) {
    sema_destroy_and_reinit_tables(sema);
    sema->file_node = file;

    // Derive module search directory from the first decl's source location
    if (BUF_LEN(file->file.decls) > 0 && file->file.decls[0]->loc.file != NULL) {
        sema->module_search_dir = dir_of(sema->arena, file->file.decls[0]->loc.file);
    } else if (file->loc.file != NULL) {
        sema->module_search_dir = dir_of(sema->arena, file->loc.file);
    }

    scope_push(sema, false); // global scope
    register_all_decls(sema, file);
    return sema->err_count == 0;
}
