#include "rsg/pass/mono/mono.h"

#include "pass/resolve/_sema.h"

/**
 * @file mono.c
 * @brief Mono pass — process deferred generic fn instantiations.
 *
 * After the check pass queues GenericInst records for each call-site
 * instantiation, this pass clones the template bodies, type-checks them
 * with concrete substitutions, and appends the specialized decls.
 */

// ── Static helpers ─────────────────────────────────────────────────

/** Clone, check, and emit all pending generic fn instantiations. */
static void instantiate_pending_generics(Sema *sema, ASTNode *file) {
    // Append synthetic decls from generic struct/enum instantiation
    for (int32_t i = 0; i < BUF_LEN(sema->synthetic_decls); i++) {
        BUF_PUSH(file->file.decls, sema->synthetic_decls[i]);
    }

    for (int32_t gi = 0; gi < BUF_LEN(sema->pending_insts); gi++) {
        GenericInst *inst = &sema->pending_insts[gi];
        GenericFnDef *gdef = inst->generic;

        // Push type param substitutions
        for (int32_t ti = 0; ti < gdef->type_param_count; ti++) {
            hash_table_insert(&sema->generics.type_params, gdef->type_params[ti].name,
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
            np->param.is_variadic = op->param.is_variadic;
            BUF_PUSH(clone->fn_decl.params, np);
        }

        // Copy return type (resolve_ast_type will use type_param_table)
        clone->fn_decl.return_type = orig->fn_decl.return_type;
        clone->fn_decl.body = ast_clone(sema->arena, orig->fn_decl.body);

        // Type-check the cloned fn body using the substitution context
        sema->fn_body_checker(sema, clone);

        // Append to file decls so lowering/codegen can see it
        BUF_PUSH(inst->file_node->file.decls, clone);

        // Clear type param substitutions
        sema_reset_type_params(sema);
    }
}

// ── Public API ─────────────────────────────────────────────────────────

bool sema_mono(Sema *sema, ASTNode *file, FnBodyChecker checker) {
    sema->fn_body_checker = checker;
    instantiate_pending_generics(sema, file);
    scope_pop(sema); // global scope (pushed by sema_resolve)
    return sema->err_count == 0;
}
