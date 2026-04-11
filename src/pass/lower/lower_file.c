#include "_lower.h"

/**
 * @file lower_file.c
 * @brief File and module level lowering — preregistration, flattening, and top-level structure.
 *
 * Separated from lower_stmt.c so that the file/module level orchestration
 * (symbol preregistration, module flattening, struct/enum method attachment)
 * does not mix with individual statement lowering.
 */

// ── Preregistration helpers ───────────────────────────────────────────

/** Pre-register method syms for a named type (struct or enum). */
static void preregister_type_methods(Lower *low, const char *type_name, ASTNode *const *methods) {
    for (int32_t j = 0; j < BUF_LEN(methods); j++) {
        const ASTNode *method = methods[j];
        const char *method_name = method->fn_decl.name;
        const Type *ret = method->type != NULL ? method->type : &TYPE_UNIT_INST;
        const char *key = arena_sprintf(low->hir_arena, "%s.%s", type_name, method_name);

        if (method->fn_decl.is_declare) {
            // Register declare methods with default mangling
            // so normal call lowering can resolve their symbol.
            const char *mangled =
                arena_sprintf(low->hir_arena, "rsgu_%s_%s",
                              lower_mangle_name(low->hir_arena, type_name), method_name);
            HirSymSpec sym_spec = {HIR_SYM_FN, key, ret, false, method->loc};
            HirSym *sym = lower_make_sym(low, &sym_spec);
            sym->mangled_name = mangled;
            sym->is_ptr_recv = method->fn_decl.is_ptr_recv;
            lower_scope_define(low, key, sym);
            continue;
        }

        const char *mangled =
            arena_sprintf(low->hir_arena, "rsgu_%s_%s",
                          lower_mangle_name(low->hir_arena, type_name), method_name);
        HirSymSpec sym_spec = {HIR_SYM_FN, key, ret, false, method->loc};
        HirSym *sym = lower_make_sym(low, &sym_spec);
        sym->mangled_name = mangled;
        sym->is_ptr_recv = method->fn_decl.is_ptr_recv;
        lower_scope_define(low, key, sym);
    }
}

/** Forward declaration for recursive module preregistration. */
static void preregister_decl_list(Lower *low, ASTNode *const *decls, int32_t count);

/** Pre-register a single decl (fn/struct/enum/ext/module). */
static void preregister_single_decl(Lower *low, const ASTNode *decl) {
    if (decl->kind == NODE_FN_DECL) {
        if (BUF_LEN(decl->fn_decl.type_params) > 0) {
            return;
        }
        if (decl->fn_decl.is_declare) {
            // Register declare fns with default "rsgu_" mangling.
            // Intrinsic builtins (print, assert, etc.) are still expanded by the lower
            // pass, but non-intrinsic declare fns fall through to normal call lowering
            // and need a correctly mangled symbol in scope.
            const char *bare = strrchr(decl->fn_decl.name, '.');
            bare = (bare != NULL) ? bare + 1 : decl->fn_decl.name;
            IntrinsicKind ik = intrinsic_lookup(bare);
            if (ik == INTRINSIC_NONE) {
                const Type *ret = decl->type != NULL ? decl->type : &TYPE_UNIT_INST;
                HirSymSpec fn_spec = {HIR_SYM_FN, decl->fn_decl.name, ret, false, decl->loc};
                HirSym *sym = lower_make_sym(low, &fn_spec);
                sym->mangled_name =
                    arena_sprintf(low->hir_arena, "rsgu_%s",
                                  lower_mangle_name(low->hir_arena, decl->fn_decl.name));
                lower_scope_define(low, decl->fn_decl.name, sym);
            }
            return;
        }
        const Type *ret = decl->type != NULL ? decl->type : &TYPE_UNIT_INST;
        HirSymSpec fn_spec = {HIR_SYM_FN, decl->fn_decl.name, ret, false, decl->loc};
        HirSym *sym = lower_make_sym(low, &fn_spec);
        sym->mangled_name = arena_sprintf(low->hir_arena, "rsgu_%s",
                                          lower_mangle_name(low->hir_arena, decl->fn_decl.name));
        lower_scope_define(low, decl->fn_decl.name, sym);
    }
    if (decl->kind == NODE_STRUCT_DECL) {
        if (BUF_LEN(decl->struct_decl->type_params) > 0) {
            return;
        }
        preregister_type_methods(low, decl->struct_decl->name, decl->struct_decl->methods);
    }
    if (decl->kind == NODE_ENUM_DECL) {
        if (BUF_LEN(decl->enum_decl->type_params) > 0) {
            return;
        }
        preregister_type_methods(low, decl->enum_decl->name, decl->enum_decl->methods);
    }
    if (decl->kind == NODE_EXT_DECL) {
        if (BUF_LEN(decl->ext_decl->type_params) > 0) {
            return;
        }
        preregister_type_methods(low, decl->ext_decl->target_name, decl->ext_decl->methods);
    }
    if (decl->kind == NODE_MODULE && decl->module.decls != NULL) {
        preregister_decl_list(low, decl->module.decls, BUF_LEN(decl->module.decls));
    }
    if (decl->kind == NODE_USE_DECL) {
        const char *mod_path = decl->use_decl.module_path;

        // Wildcard import: scan all scope entries with "mod_path." prefix
        if (decl->use_decl.is_wildcard) {
            const char *prefix_dot = arena_sprintf(low->hir_arena, "%s.", mod_path);
            size_t prefix_len = strlen(prefix_dot);
            for (LoweringScope *sc = low->scope; sc != NULL; sc = sc->parent) {
                for (int32_t i = 0; i < sc->table.capacity; i++) {
                    HashEntry *e = &sc->table.entries[i];
                    if (e->key == NULL || e->key == (const char *)(uintptr_t)1) {
                        continue;
                    }
                    if (strncmp(e->key, prefix_dot, prefix_len) == 0) {
                        const char *bare = e->key + prefix_len;
                        if (strchr(bare, '.') != NULL) {
                            continue;
                        }
                        HirSym *sym = (HirSym *)e->value;
                        lower_scope_define(low, bare, sym);
                    }
                }
            }
        }

        for (int32_t i = 0; i < BUF_LEN(decl->use_decl.imported_names); i++) {
            const char *name = decl->use_decl.imported_names[i];
            const char *alias =
                (decl->use_decl.aliases != NULL && i < BUF_LEN(decl->use_decl.aliases) &&
                 decl->use_decl.aliases[i] != NULL)
                    ? decl->use_decl.aliases[i]
                    : name;
            const char *qualified;
            if (mod_path[0] == '\0') {
                qualified = name;
            } else {
                qualified = arena_sprintf(low->hir_arena, "%s.%s", mod_path, name);
            }
            HirSym *target_sym = lower_scope_lookup(low, qualified);
            if (target_sym != NULL) {
                lower_scope_define(low, alias, target_sym);
            }
        }
    }
}

static void preregister_decl_list(Lower *low, ASTNode *const *decls, int32_t count) {
    for (int32_t i = 0; i < count; i++) {
        preregister_single_decl(low, decls[i]);
    }
}

/** Pre-register all fn decls into scope before lower bodies. */
static void preregister_fns(Lower *low, const ASTNode *file_ast) {
    preregister_decl_list(low, file_ast->file.decls, BUF_LEN(file_ast->file.decls));
}

// ── Method and type emission ──────────────────────────────────────────

/** Lower methods and append to @p decls. */
static void lower_methods_into(Lower *low, ASTNode *const *methods, const char *type_name,
                               const Type *type, HirNode ***decls) {
    for (int32_t j = 0; j < BUF_LEN(methods); j++) {
        HirNode *method = lower_method_decl(low, methods[j], type_name, type);
        if (method != NULL) {
            BUF_PUSH(*decls, method);
        }
    }
}

/** Emit a struct type decl and its methods into @p decls. */
static void emit_struct_with_methods(Lower *low, const ASTNode *decl_ast, HirNode ***decls) {
    HirNode *struct_decl = hir_new(low->hir_arena, HIR_STRUCT_DECL, &TYPE_UNIT_INST, decl_ast->loc);
    struct_decl->struct_decl.name = decl_ast->struct_decl->name;
    struct_decl->struct_decl.struct_type = decl_ast->type;
    BUF_PUSH(*decls, struct_decl);

    lower_methods_into(low, decl_ast->struct_decl->methods, decl_ast->struct_decl->name,
                       decl_ast->type, decls);
}

/** Emit an enum type decl and its methods into @p decls. */
static void emit_enum_with_methods(Lower *low, const ASTNode *decl_ast, HirNode ***decls) {
    HirNode *enum_decl = hir_new(low->hir_arena, HIR_ENUM_DECL, &TYPE_UNIT_INST, decl_ast->loc);
    enum_decl->enum_decl.name = decl_ast->enum_decl->name;
    enum_decl->enum_decl.enum_type = decl_ast->type;
    BUF_PUSH(*decls, enum_decl);

    // Register enum type as compound for typedef emission
    BUF_PUSH(low->compound_types, decl_ast->type);

    lower_methods_into(low, decl_ast->enum_decl->methods, decl_ast->enum_decl->name, decl_ast->type,
                       decls);
}

// ── Module flattening ─────────────────────────────────────────────────

/** Recursively flatten module inner declarations into the flat list. */
static void flatten_module_decls(Lower *low, const ASTNode *mod, HirNode ***decls) {
    for (int32_t j = 0; j < BUF_LEN(mod->module.decls); j++) {
        const ASTNode *inner = mod->module.decls[j];
        if (inner->kind == NODE_STRUCT_DECL) {
            if (BUF_LEN(inner->struct_decl->type_params) > 0) {
                continue;
            }
            emit_struct_with_methods(low, inner, decls);
            continue;
        }
        if (inner->kind == NODE_ENUM_DECL) {
            if (BUF_LEN(inner->enum_decl->type_params) > 0) {
                continue;
            }
            emit_enum_with_methods(low, inner, decls);
            continue;
        }
        if (inner->kind == NODE_PACT_DECL) {
            continue;
        }
        if (inner->kind == NODE_EXT_DECL) {
            if (BUF_LEN(inner->ext_decl->type_params) > 0) {
                continue;
            }
            const Type *recv_type = inner->type;
            if (recv_type != NULL) {
                lower_methods_into(low, inner->ext_decl->methods, inner->ext_decl->target_name,
                                   recv_type, decls);
            }
            continue;
        }
        if (inner->kind == NODE_USE_DECL) {
            continue;
        }
        if (inner->kind == NODE_MODULE && inner->module.decls != NULL) {
            HirNode *mod_node = lower_module(low, inner);
            if (mod_node != NULL) {
                BUF_PUSH(*decls, mod_node);
            }
            flatten_module_decls(low, inner, decls);
            continue;
        }
        HirNode *inner_hir = lower_node(low, inner);
        if (inner_hir != NULL) {
            BUF_PUSH(*decls, inner_hir);
        }
    }
}

// ── File-level lowering ───────────────────────────────────────────────

HirNode *lower_file(Lower *low, const ASTNode *ast) {
    lower_scope_enter(low);
    preregister_fns(low, ast);

    HirNode **decls = NULL;
    for (int32_t i = 0; i < BUF_LEN(ast->file.decls); i++) {
        const ASTNode *decl_ast = ast->file.decls[i];

        if (decl_ast->kind == NODE_STRUCT_DECL) {
            // Skip generic struct templates — only monomorphized copies are lowered
            if (BUF_LEN(decl_ast->struct_decl->type_params) > 0) {
                continue;
            }
            emit_struct_with_methods(low, decl_ast, &decls);
            continue;
        }

        if (decl_ast->kind == NODE_ENUM_DECL) {
            // Skip generic enum templates — only monomorphized copies are lowered
            if (BUF_LEN(decl_ast->enum_decl->type_params) > 0) {
                continue;
            }
            emit_enum_with_methods(low, decl_ast, &decls);
            continue;
        }

        // Pact decls are compile-time only; skip during lower
        if (decl_ast->kind == NODE_PACT_DECL) {
            continue;
        }

        // Ext decls: lower methods, skip the ext node itself
        if (decl_ast->kind == NODE_EXT_DECL) {
            if (BUF_LEN(decl_ast->ext_decl->type_params) > 0) {
                continue;
            }
            const char *target = decl_ast->ext_decl->target_name;
            const Type *recv_type = decl_ast->type;
            if (recv_type != NULL) {
                lower_methods_into(low, decl_ast->ext_decl->methods, target, recv_type, &decls);
            }
            continue;
        }

        // Use decls are compile-time only; skip during lower
        if (decl_ast->kind == NODE_USE_DECL) {
            continue;
        }

        // Nested module: recursively flatten inner declarations
        if (decl_ast->kind == NODE_MODULE && decl_ast->module.decls != NULL) {
            HirNode *mod_node = lower_module(low, decl_ast);
            if (mod_node != NULL) {
                BUF_PUSH(decls, mod_node);
            }
            flatten_module_decls(low, decl_ast, &decls);
            continue;
        }

        HirNode *decl = lower_node(low, decl_ast);
        if (decl != NULL) {
            BUF_PUSH(decls, decl);
        }
    }
    lower_scope_leave(low);

    HirNode *file_node = hir_new(low->hir_arena, HIR_FILE, &TYPE_UNIT_INST, ast->loc);
    file_node->file.decls = decls;
    return file_node;
}

HirNode *lower_module(Lower *low, const ASTNode *ast) {
    low->current_module = ast->module.name;
    HirNode *node = hir_new(low->hir_arena, HIR_MODULE, &TYPE_UNIT_INST, ast->loc);
    node->module.name = ast->module.name;
    return node;
}
