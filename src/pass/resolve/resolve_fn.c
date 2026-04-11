#include "_sema.h"

/**
 * @file resolve_fn.c
 * @brief Fn signature building and registration.
 *
 * Provides build_fn_sig (used across passes for fn/method sig creation),
 * register_fn_sig, and register_method_sig.
 */

// ── Fn signature helpers ───────────────────────────────────────────

FnSig *build_fn_sig(Sema *sema, ASTNode *decl, bool is_pub) {
    const Type *return_type = &TYPE_UNIT_INST;
    if (decl->fn_decl.return_type.kind != AST_TYPE_INFERRED) {
        return_type = resolve_ast_type(sema, &decl->fn_decl.return_type);
        if (return_type == NULL) {
            return_type = &TYPE_UNIT_INST;
        }
    }

    FnSig *sig = rsg_malloc(sizeof(*sig));
    sig->name = decl->fn_decl.name;
    sig->return_type = return_type;
    sig->param_types = NULL;
    sig->param_names = NULL;
    sig->param_count = BUF_LEN(decl->fn_decl.params);
    sig->is_pub = is_pub;
    sig->is_declare = decl->fn_decl.is_declare;
    sig->has_variadic = false;
    sig->intrinsic = intrinsic_lookup(decl->fn_decl.name);

    for (int32_t j = 0; j < sig->param_count; j++) {
        ASTNode *param = decl->fn_decl.params[j];
        const Type *pt = resolve_ast_type(sema, &param->param.type);
        if (pt == NULL) {
            pt = &TYPE_ERR_INST;
        }
        // Variadic param: ..T → []T (slice type)
        if (param->param.is_variadic) {
            pt = type_create_slice(sema->base.arena, pt);
            sig->has_variadic = true;
        }
        BUF_PUSH(sig->param_types, pt);
        BUF_PUSH(sig->param_names, param->param.name);
    }
    return sig;
}

void register_method_sig(Sema *sema, const char *type_name, ASTNode *method,
                         StructMethodInfo **methods) {
    StructMethodInfo mi = {.name = method->fn_decl.name,
                           .is_mut_recv = method->fn_decl.is_mut_recv,
                           .is_ptr_recv = method->fn_decl.is_ptr_recv,
                           .recv_name = method->fn_decl.recv_name,
                           .decl = method};
    BUF_PUSH(*methods, mi);

    const char *method_key = arena_sprintf(sema->base.arena, "%s.%s", type_name, mi.name);
    FnSig *sig = build_fn_sig(sema, method, false);
    sig->is_ptr_recv = method->fn_decl.is_ptr_recv;
    hash_table_insert(&sema->base.db.fn_table, method_key, sig);
}

// ── Registration ───────────────────────────────────────────────────

void register_fn_sig(Sema *sema, ASTNode *decl) {
    // If the fn has type params, store as a generic template instead
    if (BUF_LEN(decl->fn_decl.type_params) > 0) {
        // Reject default generics on functions
        for (int32_t i = 0; i < BUF_LEN(decl->fn_decl.type_params); i++) {
            if (decl->fn_decl.type_params[i].default_type != NULL) {
                SEMA_ERR(sema, decl->loc, "default generics are not allowed on fn declarations");
                return;
            }
        }
        GenericFnDef *gdef = rsg_malloc(sizeof(*gdef));
        gdef->name = decl->fn_decl.name;
        gdef->decl = decl;
        gdef->type_params = decl->fn_decl.type_params;
        gdef->type_param_count = BUF_LEN(decl->fn_decl.type_params);
        hash_table_insert(&sema->base.generics.fn, decl->fn_decl.name, gdef);
        return;
    }

    FnSig *sig = build_fn_sig(sema, decl, decl->fn_decl.is_pub);
    hash_table_insert(&sema->base.db.fn_table, decl->fn_decl.name, sig);

    scope_define(sema,
                 &(SymDef){decl->fn_decl.name, sig->return_type, decl->fn_decl.is_pub, SYM_FN});
}
