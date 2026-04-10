#include "rsg/pass/mono/mono.h"

#include <string.h>

#include "pass/resolve/_sema.h"

/**
 * @file mono.c
 * @brief Mono pass — process deferred generic fn instantiations.
 *
 * After the check pass queues GenericInst records for each call-site
 * instantiation, this pass clones the template bodies, type-checks them
 * with concrete substitutions, and appends the specialized decls.
 */

#define MONO_DEPTH_LIMIT 64

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

        // Guard: detect duplicate mangled names (prevents redundant instantiation)
        bool duplicate = false;
        for (int32_t prev = 0; prev < gi; prev++) {
            if (strcmp(sema->pending_insts[prev].mangled_name, inst->mangled_name) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }

        // Guard: enforce recursion depth limit for monomorphization
        if (sema->mono_depth >= MONO_DEPTH_LIMIT) {
            SEMA_ERR(sema, gdef->decl->loc,
                     "generic instantiation depth limit (%d) exceeded for '%s'", MONO_DEPTH_LIMIT,
                     inst->mangled_name);
            continue;
        }

        // Snapshot inst fields before fn_body_checker, which may realloc pending_insts
        const char *mangled_name = inst->mangled_name;
        const Type **type_args = inst->type_args;
        ASTNode *file_node = inst->file_node;

        // Push type param substitutions (save previous for nested generics)
        const Type **saved = NULL;
        sema_push_type_params(sema, gdef->type_params, type_args, gdef->type_param_count, &saved);

        // Create a cloned fn_decl with the mangled name and concrete types
        ASTNode *orig = gdef->decl;
        ASTNode *clone = ast_new(sema->base.arena, NODE_FN_DECL, orig->loc);
        clone->fn_decl.is_pub = false;
        clone->fn_decl.name = mangled_name;
        clone->fn_decl.params = NULL;
        clone->fn_decl.type_params = NULL;
        clone->fn_decl.recv_name = NULL;
        clone->fn_decl.is_mut_recv = false;
        clone->fn_decl.is_ptr_recv = false;
        clone->fn_decl.owner_struct = NULL;

        // Clone params with substituted types
        for (int32_t pi = 0; pi < BUF_LEN(orig->fn_decl.params); pi++) {
            ASTNode *op = orig->fn_decl.params[pi];
            ASTNode *np = ast_new(sema->base.arena, NODE_PARAM, op->loc);
            np->param.name = op->param.name;
            np->param.type = op->param.type;
            np->param.is_mut = op->param.is_mut;
            np->param.is_variadic = op->param.is_variadic;
            BUF_PUSH(clone->fn_decl.params, np);
        }

        // Copy return type (resolve_ast_type will use type_param_table)
        clone->fn_decl.return_type = orig->fn_decl.return_type;
        clone->fn_decl.body = ast_clone(sema->base.arena, orig->fn_decl.body);

        // Reset closure context to prevent state leakage between instantiations.
        // Without this, a previous generic fn's closure state (e.g. captures_mutated)
        // can pollute subsequent instantiation type-checking.
        sema->closure = (ClosureCtx){0};

        // Type-check the cloned fn body using the substitution context
        sema->mono_depth++;
        sema->fn_body_checker(sema, clone);
        sema->mono_depth--;

        // Append to file decls so lowering/codegen can see it
        BUF_PUSH(file_node->file.decls, clone);

        // Pop type param substitutions (restore previous values)
        sema_pop_type_params(sema, gdef->type_params, gdef->type_param_count, saved);
    }
}

// ── Public API ─────────────────────────────────────────────────────────

bool sema_mono(Sema *sema, ASTNode *file, FnBodyChecker checker) {
    sema->base.phase = SEMA_PHASE_MONO;
    sema->fn_body_checker = checker;
    instantiate_pending_generics(sema, file);
    scope_pop(sema); // global scope (pushed by sema_resolve)
    return sema->base.err_count == 0;
}
