#include "_sema.h"

#ifdef _WIN32
#define PATH_SEP '\\'
#else
#define PATH_SEP '/'
#endif

/**
 * @file resolve_module.c
 * @brief Module and use-decl registration.
 *
 * Handles filesystem module loading, qualified name rewriting,
 * wildcard imports, and selective use-decl resolution.
 */

// ── Static helpers ─────────────────────────────────────────────────

/** Resolve a 'super' or 'super::super' path to the parent module's qualified name. */
static const char *resolve_super_path(Sema *sema, const char *path, SrcLoc loc) {
    const char *current = sema->base.current_scope->module_name;
    if (current == NULL) {
        // At file scope, super:: has no parent
        SEMA_ERR(sema, loc, "'super' used outside of a module");
        return NULL;
    }
    // Walk up one level for each 'super' segment (separated by '::')
    // The path is "super" or "super::super" etc., optionally followed by "::rest"
    const char *remaining = path;
    char *cur = arena_sprintf(sema->base.arena, "%s", current);
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
                cur[0] = '\0';
            }
        } else {
            break;
        }
        remaining = (sep != NULL) ? sep + 2 : NULL;
    }
    // Append any remaining non-super segments (e.g., "math_ops" from "super::math_ops")
    if (remaining != NULL && remaining[0] != '\0') {
        if (cur[0] != '\0') {
            return arena_sprintf(sema->base.arena, "%s.%s", cur, remaining);
        }
        return remaining;
    }
    return cur;
}

/**
 * Wildcard import: iterate all pub symbols whose qualified key starts
 * with "mod_name." and import them under their bare name.
 * When @p reexport_prefix is non-NULL (pub use inside a module),
 * also register under "reexport_prefix.bare" as a pub re-export.
 */
static void register_wildcard_use(Sema *sema, const char *mod_name, const char *reexport_prefix) {
    size_t prefix_len = strlen(mod_name);
    const char *prefix_dot = arena_sprintf(sema->base.arena, "%s.", mod_name);
    size_t prefix_dot_len = prefix_len + 1;

    // Scan fn_table
    for (int32_t i = 0; i < sema->base.db.fn_table.capacity; i++) {
        HashEntry *e = &sema->base.db.fn_table.entries[i];
        if (e->key == NULL || e->key == (const char *)(uintptr_t)1) {
            continue;
        }
        if (strncmp(e->key, prefix_dot, prefix_dot_len) == 0) {
            const char *bare = e->key + prefix_dot_len;
            // Skip sub-module qualified names (contain another '.')
            if (strchr(bare, '.') != NULL) {
                continue;
            }
            FnSig *sig = (FnSig *)e->value;
            if (sig->is_pub) {
                hash_table_insert(&sema->base.db.fn_table, bare, sig);
                if (reexport_prefix != NULL) {
                    const char *rekey =
                        arena_sprintf(sema->base.arena, "%s.%s", reexport_prefix, bare);
                    hash_table_insert(&sema->base.db.fn_table, rekey, sig);
                }
            }
        }
    }

    // Scan struct_table
    for (int32_t i = 0; i < sema->base.db.struct_table.capacity; i++) {
        HashEntry *e = &sema->base.db.struct_table.entries[i];
        if (e->key == NULL || e->key == (const char *)(uintptr_t)1) {
            continue;
        }
        if (strncmp(e->key, prefix_dot, prefix_dot_len) == 0) {
            const char *bare = e->key + prefix_dot_len;
            if (strchr(bare, '.') != NULL) {
                continue;
            }
            StructDef *sdef = (StructDef *)e->value;
            hash_table_insert(&sema->base.db.struct_table, bare, sdef);
            scope_define(sema, &(SymDef){bare, sdef->type, true, SYM_TYPE});
            if (reexport_prefix != NULL) {
                const char *rekey = arena_sprintf(sema->base.arena, "%s.%s", reexport_prefix, bare);
                hash_table_insert(&sema->base.db.struct_table, rekey, sdef);
                scope_define(sema, &(SymDef){rekey, sdef->type, true, SYM_TYPE});
            }
        }
    }

    // Scan enum_table
    for (int32_t i = 0; i < sema->base.db.enum_table.capacity; i++) {
        HashEntry *e = &sema->base.db.enum_table.entries[i];
        if (e->key == NULL || e->key == (const char *)(uintptr_t)1) {
            continue;
        }
        if (strncmp(e->key, prefix_dot, prefix_dot_len) == 0) {
            const char *bare = e->key + prefix_dot_len;
            if (strchr(bare, '.') != NULL) {
                continue;
            }
            EnumDef *edef = (EnumDef *)e->value;
            hash_table_insert(&sema->base.db.enum_table, bare, edef);
            scope_define(sema, &(SymDef){bare, edef->type, true, SYM_TYPE});
            if (reexport_prefix != NULL) {
                const char *rekey = arena_sprintf(sema->base.arena, "%s.%s", reexport_prefix, bare);
                hash_table_insert(&sema->base.db.enum_table, rekey, edef);
                scope_define(sema, &(SymDef){rekey, edef->type, true, SYM_TYPE});
            }
        }
    }

    // Scan type_alias_table
    for (int32_t i = 0; i < sema->base.db.type_alias_table.capacity; i++) {
        HashEntry *e = &sema->base.db.type_alias_table.entries[i];
        if (e->key == NULL || e->key == (const char *)(uintptr_t)1) {
            continue;
        }
        if (strncmp(e->key, prefix_dot, prefix_dot_len) == 0) {
            const char *bare = e->key + prefix_dot_len;
            if (strchr(bare, '.') != NULL) {
                continue;
            }
            hash_table_insert(&sema->base.db.type_alias_table, bare, e->value);
            if (reexport_prefix != NULL) {
                const char *rekey = arena_sprintf(sema->base.arena, "%s.%s", reexport_prefix, bare);
                hash_table_insert(&sema->base.db.type_alias_table, rekey, e->value);
            }
        }
    }
}

// ── Public API ─────────────────────────────────────────────────────

void register_module_decl(Sema *sema, ASTNode *decl) {
    const char *mod_name = decl->module.name;

    // Create module type and register in scope
    const Type *mod_type = type_create_module(sema->base.arena, mod_name);
    scope_define(sema, &(SymDef){mod_name, mod_type, true, SYM_MODULE});

    if (decl->module.decls == NULL) {
        return;
    }

    // Set module context so resolve_ast_type can find sibling types
    const char *prev_module_name = sema->base.current_scope->module_name;
    sema->base.current_scope->module_name = mod_name;

    // Register all inner declarations with qualified names
    for (int32_t i = 0; i < BUF_LEN(decl->module.decls); i++) {
        ASTNode *inner = decl->module.decls[i];

        if (inner->kind == NODE_MODULE) {
            // Nested sub-module: prefix its name and recurse
            const char *orig_name = inner->module.name;
            inner->module.name = arena_sprintf(sema->base.arena, "%s.%s", mod_name, orig_name);

            // Filesystem module: load from module_search_dir/<orig_name>.rsg
            if (inner->module.decls == NULL && sema->module.search_dir != NULL) {
                const char *mod_path = arena_sprintf(sema->base.arena, "%s%c%s.rsg",
                                                     sema->module.search_dir, PATH_SEP, orig_name);
                ASTNode **decls = load_module_decls(sema, mod_path);

                // Fallback: try std library directory
                const char *base_dir = sema->module.search_dir;
                if (decls == NULL && sema->module.std_dir != NULL) {
                    mod_path = arena_sprintf(sema->base.arena, "%s%c%s.rsg", sema->module.std_dir,
                                             PATH_SEP, orig_name);
                    decls = load_module_decls(sema, mod_path);
                    if (decls != NULL) {
                        base_dir = sema->module.std_dir;
                    }
                }

                if (decls == NULL) {
                    SEMA_ERR(sema, inner->loc, "cannot find module file '%s.rsg'", orig_name);
                } else {
                    inner->module.decls = decls;
                    // Update search dir for grandchild modules
                    const char *prev_dir = sema->module.search_dir;
                    sema->module.search_dir =
                        arena_sprintf(sema->base.arena, "%s%c%s", base_dir, PATH_SEP, orig_name);
                    register_module_decl(sema, inner);
                    sema->module.search_dir = prev_dir;
                    continue;
                }
            }

            register_module_decl(sema, inner);
            continue;
        }

        if (inner->kind == NODE_FN_DECL) {
            // Rewrite fn name to qualified form: mod.fn_name
            const char *qualified =
                arena_sprintf(sema->base.arena, "%s.%s", mod_name, inner->fn_decl.name);
            inner->fn_decl.name = qualified;
            register_fn_sig(sema, inner);
            continue;
        }

        if (inner->kind == NODE_STRUCT_DECL) {
            const char *qualified =
                arena_sprintf(sema->base.arena, "%s.%s", mod_name, inner->struct_decl->name);
            inner->struct_decl->name = qualified;
            register_struct_def(sema, inner);
            continue;
        }

        if (inner->kind == NODE_ENUM_DECL) {
            const char *qualified =
                arena_sprintf(sema->base.arena, "%s.%s", mod_name, inner->enum_decl->name);
            inner->enum_decl->name = qualified;
            register_enum_def(sema, inner);
            continue;
        }

        if (inner->kind == NODE_PACT_DECL) {
            const char *qualified =
                arena_sprintf(sema->base.arena, "%s.%s", mod_name, inner->pact_decl->name);
            inner->pact_decl->name = qualified;
            register_pact_def(sema, inner);
            continue;
        }

        if (inner->kind == NODE_TYPE_ALIAS) {
            const char *qualified =
                arena_sprintf(sema->base.arena, "%s.%s", mod_name, inner->type_alias.name);
            inner->type_alias.name = qualified;
            const Type *underlying = resolve_ast_type(sema, &inner->type_alias.alias_type);
            if (underlying != NULL) {
                hash_table_insert(&sema->base.db.type_alias_table, qualified, (void *)underlying);
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

    sema->base.current_scope->module_name = prev_module_name;
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
        char *normalized = arena_alloc(sema->base.arena, len + 1);
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

    // Determine re-export prefix for pub use inside a module
    const char *reexport_prefix = NULL;
    if (decl->use_decl.is_pub && sema->base.current_scope->module_name != NULL) {
        reexport_prefix = sema->base.current_scope->module_name;
    }

    // Wildcard import: use module::*
    if (decl->use_decl.is_wildcard) {
        register_wildcard_use(sema, mod_name, reexport_prefix);
        return;
    }

    for (int32_t i = 0; i < BUF_LEN(decl->use_decl.imported_names); i++) {
        const char *name = decl->use_decl.imported_names[i];
        const char *alias = decl->use_decl.aliases[i];
        const char *qualified;
        if (strlen(mod_name) == 0) {
            qualified = name;
        } else {
            qualified = arena_sprintf(sema->base.arena, "%s.%s", mod_name, name);
        }

        // Try fn
        FnSig *sig = sema_lookup_fn(sema, qualified);
        if (sig != NULL) {
            // Check visibility: fn must be pub
            if (!sig->is_pub) {
                SEMA_ERR(sema, decl->loc, "'%s' is private in module '%s'", name, mod_name);
                continue;
            }
            hash_table_insert(&sema->base.db.fn_table, alias, sig);
            if (reexport_prefix != NULL) {
                const char *rekey =
                    arena_sprintf(sema->base.arena, "%s.%s", reexport_prefix, alias);
                hash_table_insert(&sema->base.db.fn_table, rekey, sig);
            }
            continue;
        }

        // Try struct
        StructDef *sdef = sema_lookup_struct(sema, qualified);
        if (sdef != NULL) {
            hash_table_insert(&sema->base.db.struct_table, alias, sdef);
            scope_define(sema, &(SymDef){alias, sdef->type, true, SYM_TYPE});
            if (reexport_prefix != NULL) {
                const char *rekey =
                    arena_sprintf(sema->base.arena, "%s.%s", reexport_prefix, alias);
                hash_table_insert(&sema->base.db.struct_table, rekey, sdef);
                scope_define(sema, &(SymDef){rekey, sdef->type, true, SYM_TYPE});
            }
            continue;
        }

        // Try enum
        EnumDef *edef = sema_lookup_enum(sema, qualified);
        if (edef != NULL) {
            hash_table_insert(&sema->base.db.enum_table, alias, edef);
            scope_define(sema, &(SymDef){alias, edef->type, true, SYM_TYPE});
            if (reexport_prefix != NULL) {
                const char *rekey =
                    arena_sprintf(sema->base.arena, "%s.%s", reexport_prefix, alias);
                hash_table_insert(&sema->base.db.enum_table, rekey, edef);
                scope_define(sema, &(SymDef){rekey, edef->type, true, SYM_TYPE});
            }
            continue;
        }

        // Try type alias
        const Type *talias = sema_lookup_type_alias(sema, qualified);
        if (talias != NULL) {
            hash_table_insert(&sema->base.db.type_alias_table, alias, (void *)talias);
            if (reexport_prefix != NULL) {
                const char *rekey =
                    arena_sprintf(sema->base.arena, "%s.%s", reexport_prefix, alias);
                hash_table_insert(&sema->base.db.type_alias_table, rekey, (void *)talias);
            }
            continue;
        }

        SEMA_ERR(sema, decl->loc, "'%s' not found in module '%s'", name, mod_name);
    }
}
