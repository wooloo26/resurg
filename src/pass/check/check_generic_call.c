#include "_check.h"

/**
 * @file check_generic_call.c
 * @brief Generic fn call resolution — explicit type args and type inference.
 */

// ── Type arg resolution ───────────────────────────────────────────

/** Resolve type args for a generic fn call and push substitutions. */
static const Type **resolve_generic_type_args(Sema *sema, ASTNode *node, GenericFnDef *gdef,
                                              const Type ***out_saved) {
    int32_t got = BUF_LEN(node->call.type_args);
    const Type **resolved_args = NULL;
    for (int32_t i = 0; i < got; i++) {
        const Type *t = resolve_ast_type(sema, &node->call.type_args[i]);
        if (t == NULL) {
            t = &TYPE_ERR_INST;
        }
        BUF_PUSH(resolved_args, t);
    }

    // Push type param substitutions (save previous values for nested generics)
    sema_push_type_params(sema, gdef->type_params, resolved_args, gdef->type_param_count,
                          out_saved);
    return resolved_args;
}

// ── Bound validation ──────────────────────────────────────────────

/** Validate type params against their declared pact bounds. */
static bool validate_pact_bounds(Sema *sema, ASTNode *node, GenericFnDef *gdef,
                                 const Type **resolved_args) {
    int32_t expected = gdef->type_param_count;
    for (int32_t i = 0; i < expected; i++) {
        for (int32_t b = 0; b < BUF_LEN(gdef->type_params[i].bounds); b++) {
            const char *bound = gdef->type_params[i].bounds[b];
            if (!type_satisfies_bound(sema, resolved_args[i], bound)) {
                SEMA_ERR(sema, node->loc, "'%s' does not satisfy pact bound '%s'",
                         type_name(sema->base.arena, resolved_args[i]), bound);
                return false;
            }
        }
    }
    return true;
}

/** Validate associated type constraints (e.g. T: Pact<Item = i32>). */
static bool validate_assoc_constraints(Sema *sema, ASTNode *node, GenericFnDef *gdef,
                                       const Type **resolved_args) {
    int32_t expected = gdef->type_param_count;
    for (int32_t i = 0; i < expected; i++) {
        for (int32_t c = 0; c < BUF_LEN(gdef->type_params[i].assoc_constraints); c++) {
            ASTAssocConstraint *ac = &gdef->type_params[i].assoc_constraints[c];
            if (resolved_args[i]->kind != TYPE_STRUCT) {
                continue;
            }
            StructDef *sdef = sema_lookup_struct(sema, resolved_args[i]->struct_type.name);
            if (sdef == NULL) {
                continue;
            }
            const Type *assoc_resolved = NULL;
            for (int32_t a = 0; a < BUF_LEN(sdef->assoc_types); a++) {
                if (strcmp(sdef->assoc_types[a].name, ac->assoc_name) == 0 &&
                    sdef->assoc_types[a].concrete_type != NULL) {
                    assoc_resolved = resolve_ast_type(sema, sdef->assoc_types[a].concrete_type);
                    break;
                }
            }
            if (assoc_resolved == NULL || assoc_resolved->kind == TYPE_ERR) {
                SEMA_ERR(sema, node->loc, "'%s' has no associated type '%s' for pact '%s'",
                         type_name(sema->base.arena, resolved_args[i]), ac->assoc_name,
                         ac->pact_name);
                return false;
            }
            const Type *expected_type = resolve_ast_type(sema, ac->expected_type);
            if (expected_type != NULL && expected_type->kind != TYPE_ERR &&
                !type_equal(assoc_resolved, expected_type)) {
                SEMA_ERR(sema, node->loc, "associated type '%s' is '%s', expected '%s'",
                         ac->assoc_name, type_name(sema->base.arena, assoc_resolved),
                         type_name(sema->base.arena, expected_type));
                return false;
            }
        }
    }
    return true;
}

/** Validate where clause bounds (including association type projections). */
static bool validate_where_clauses(Sema *sema, ASTNode *node, GenericFnDef *gdef,
                                   const Type **resolved_args) {
    int32_t expected = gdef->type_param_count;
    ASTWhereClause *wcs = gdef->decl->fn_decl.where_clauses;
    for (int32_t w = 0; w < BUF_LEN(wcs); w++) {
        const Type *wc_type = NULL;
        for (int32_t i = 0; i < expected; i++) {
            if (strcmp(gdef->type_params[i].name, wcs[w].type_name) == 0) {
                wc_type = resolved_args[i];
                break;
            }
        }
        if (wc_type == NULL) {
            continue;
        }
        // Handle associated type projection: I::Item: Clone
        if (wcs[w].assoc_member != NULL) {
            if (wc_type->kind == TYPE_STRUCT) {
                StructDef *sdef = sema_lookup_struct(sema, wc_type->struct_type.name);
                if (sdef != NULL) {
                    const Type *assoc_resolved = NULL;
                    for (int32_t a = 0; a < BUF_LEN(sdef->assoc_types); a++) {
                        if (strcmp(sdef->assoc_types[a].name, wcs[w].assoc_member) == 0 &&
                            sdef->assoc_types[a].concrete_type != NULL) {
                            assoc_resolved =
                                resolve_ast_type(sema, sdef->assoc_types[a].concrete_type);
                            break;
                        }
                    }
                    if (assoc_resolved != NULL && assoc_resolved->kind != TYPE_ERR) {
                        for (int32_t b = 0; b < BUF_LEN(wcs[w].bounds); b++) {
                            if (!type_satisfies_bound(sema, assoc_resolved, wcs[w].bounds[b])) {
                                SEMA_ERR(sema, node->loc,
                                         "'%s::%s' = '%s' does not satisfy pact bound '%s'",
                                         wcs[w].type_name, wcs[w].assoc_member,
                                         type_name(sema->base.arena, assoc_resolved),
                                         wcs[w].bounds[b]);
                                return false;
                            }
                        }
                    }
                }
            }
            continue;
        }
        for (int32_t b = 0; b < BUF_LEN(wcs[w].bounds); b++) {
            if (!type_satisfies_bound(sema, wc_type, wcs[w].bounds[b])) {
                SEMA_ERR(sema, node->loc, "'%s' does not satisfy pact bound '%s'",
                         type_name(sema->base.arena, wc_type), wcs[w].bounds[b]);
                return false;
            }
        }
    }
    return true;
}

/** Validate all generic bounds: pact bounds, assoc constraints, and where clauses. */
static bool validate_generic_bounds(Sema *sema, ASTNode *node, GenericFnDef *gdef,
                                    const Type **resolved_args) {
    return validate_pact_bounds(sema, node, gdef, resolved_args) &&
           validate_assoc_constraints(sema, node, gdef, resolved_args) &&
           validate_where_clauses(sema, node, gdef, resolved_args);
}

// ── Generic fn instantiation ──────────────────────────────────────

/** Resolved type arguments, mangled name, and saved params for a generic fn instantiation. */
typedef struct {
    const Type **type_args;
    const char *mangled_name;
    const Type **saved_params;
} GenericFnInstSpec;

/** Build a concrete FnSig from a generic template and queue a deferred instantiation. */
static const Type *emit_generic_fn_inst(Sema *sema, ASTNode *node, GenericFnDef *gdef,
                                        const GenericFnInstSpec *spec) {
    ASTNode *orig = gdef->decl;
    FnSig *sig = rsg_malloc(sizeof(*sig));
    sig->name = spec->mangled_name;
    sig->param_count = BUF_LEN(orig->fn_decl.params);
    sig->param_types = NULL;
    sig->param_names = NULL;
    sig->is_pub = false;
    sig->is_ptr_recv = false;
    sig->is_declare = orig->fn_decl.is_declare;
    sig->has_variadic = false;
    for (int32_t i = 0; i < sig->param_count; i++) {
        ASTNode *param = orig->fn_decl.params[i];
        const Type *pt = resolve_ast_type(sema, &param->param.type);
        if (pt == NULL) {
            pt = &TYPE_ERR_INST;
        }
        if (param->param.is_variadic) {
            pt = type_create_slice(sema->base.arena, pt);
            sig->has_variadic = true;
        }
        BUF_PUSH(sig->param_types, pt);
        BUF_PUSH(sig->param_names, param->param.name);
    }
    const Type *ret = resolve_ast_type(sema, &orig->fn_decl.return_type);
    sig->return_type = (ret != NULL) ? ret : &TYPE_UNIT_INST;

    sema_pop_type_params(sema, gdef->type_params, gdef->type_param_count, spec->saved_params);

    hash_table_insert(&sema->base.db.fn_table, spec->mangled_name, sig);

    // Decl fns have no body to clone/check — skip deferred instantiation.
    if (!orig->fn_decl.is_declare) {
        GenericInst inst = {.generic = gdef,
                            .mangled_name = spec->mangled_name,
                            .type_args = spec->type_args,
                            .file_node = sema->base.file_node};
        BUF_PUSH(sema->pending_insts, inst);
    }

    node->call.callee->id.name = spec->mangled_name;
    return resolve_call(sema, node, sig);
}

// ── Public API ────────────────────────────────────────────────────

const Type *check_generic_fn_call(Sema *sema, ASTNode *node, const char *fn_name) {
    GenericFnDef *gdef = sema_lookup_generic_fn(sema, fn_name);
    if (gdef == NULL) {
        SEMA_ERR(sema, node->loc, "undefined generic function '%s'", fn_name);
        return &TYPE_ERR_INST;
    }
    int32_t expected = gdef->type_param_count;
    int32_t got = BUF_LEN(node->call.type_args);
    if (got != expected) {
        SEMA_ERR(sema, node->loc, "wrong number of type arguments for '%s': expected %d, got %d",
                 fn_name, expected, got);
        return &TYPE_ERR_INST;
    }

    const Type **saved = NULL;
    const Type **resolved_args = resolve_generic_type_args(sema, node, gdef, &saved);

    if (!validate_generic_bounds(sema, node, gdef, resolved_args)) {
        sema_pop_type_params(sema, gdef->type_params, gdef->type_param_count, saved);
        return &TYPE_ERR_INST;
    }

    const char *mangled = build_mangled_name(sema, fn_name, resolved_args, got);
    FnSig *existing = sema_lookup_fn(sema, mangled);
    if (existing != NULL) {
        sema_pop_type_params(sema, gdef->type_params, gdef->type_param_count, saved);
        node->call.callee->id.name = mangled;
        node->call.type_args = NULL;
        return resolve_call(sema, node, existing);
    }

    const Type *result = emit_generic_fn_inst(sema, node, gdef,
                                              &(GenericFnInstSpec){.type_args = resolved_args,
                                                                   .mangled_name = mangled,
                                                                   .saved_params = saved});
    node->call.type_args = NULL;
    return result;
}

// ── Generic type inference ────────────────────────────────────────

/** Convert a resolved Type back to an ASTType for synthetic type arg construction. */
static ASTType type_to_ast_type(Arena *arena, const Type *type) {
    ASTType ast = {0};
    if (type == NULL) {
        ast.kind = AST_TYPE_NAME;
        ast.name = "<err>";
        return ast;
    }
    switch (type->kind) {
    case TYPE_SLICE:
        ast.kind = AST_TYPE_SLICE;
        ast.slice_elem = arena_alloc(arena, sizeof(ASTType));
        *ast.slice_elem = type_to_ast_type(arena, type->slice.elem);
        return ast;
    case TYPE_ARRAY:
        ast.kind = AST_TYPE_ARRAY;
        ast.array_elem = arena_alloc(arena, sizeof(ASTType));
        *ast.array_elem = type_to_ast_type(arena, type->array.elem);
        ast.array_size = type->array.size;
        return ast;
    case TYPE_PTR:
        ast.kind = AST_TYPE_PTR;
        ast.ptr_elem = arena_alloc(arena, sizeof(ASTType));
        *ast.ptr_elem = type_to_ast_type(arena, type->ptr.pointee);
        return ast;
    default:
        ast.kind = AST_TYPE_NAME;
        ast.name = type_name(arena, type);
        return ast;
    }
}

/**
 * Recursively infer type parameter bindings by matching an AST type
 * annotation against a concrete resolved type.
 *
 * Handles AST_TYPE_NAME (direct type param) and AST_TYPE_FN (Fn/FnMut types).
 */
static void infer_type_params(const ASTType *ast_type, const Type *concrete,
                              const GenericFnDef *gdef, const Type **inferred) {
    if (concrete == NULL || concrete->kind == TYPE_ERR) {
        return;
    }
    switch (ast_type->kind) {
    case AST_TYPE_NAME:
        for (int32_t i = 0; i < gdef->type_param_count; i++) {
            if (ast_type->name != NULL && strcmp(ast_type->name, gdef->type_params[i].name) == 0) {
                inferred[i] = concrete;
                return;
            }
        }
        break;
    case AST_TYPE_FN:
        if (concrete->kind != TYPE_FN) {
            break;
        }
        if (ast_type->fn_return_type != NULL) {
            infer_type_params(ast_type->fn_return_type, concrete->fn_type.return_type, gdef,
                              inferred);
        }
        for (int32_t i = 0;
             i < BUF_LEN(ast_type->fn_param_types) && i < concrete->fn_type.param_count; i++) {
            infer_type_params(ast_type->fn_param_types[i], concrete->fn_type.params[i], gdef,
                              inferred);
        }
        break;
    case AST_TYPE_PTR:
        if (concrete->kind == TYPE_PTR && ast_type->ptr_elem != NULL) {
            infer_type_params(ast_type->ptr_elem, concrete->ptr.pointee, gdef, inferred);
        }
        break;
    case AST_TYPE_SLICE:
        if (concrete->kind == TYPE_SLICE && ast_type->slice_elem != NULL) {
            infer_type_params(ast_type->slice_elem, concrete->slice.elem, gdef, inferred);
        }
        break;
    case AST_TYPE_ARRAY:
        if (concrete->kind == TYPE_ARRAY && ast_type->array_elem != NULL) {
            infer_type_params(ast_type->array_elem, concrete->array.elem, gdef, inferred);
        }
        break;
    default:
        break;
    }
}

const Type *infer_generic_call(Sema *sema, ASTNode *node, const char *fn_name) {
    GenericFnDef *gdef = sema_lookup_generic_fn(sema, fn_name);
    if (gdef == NULL) {
        return NULL;
    }
    ASTNode *orig = gdef->decl;
    int32_t num_params = gdef->type_param_count;
    int32_t arg_count = BUF_LEN(node->call.args);
    int32_t param_count = BUF_LEN(orig->fn_decl.params);
    if (arg_count != param_count) {
        return NULL;
    }

    const Type **inferred =
        (const Type **)arena_alloc_zero(sema->base.arena, num_params * sizeof(const Type *));
    for (int32_t pi = 0; pi < param_count; pi++) {
        ASTNode *param = orig->fn_decl.params[pi];
        const Type *arg_type = node->call.args[pi]->type;
        infer_type_params(&param->param.type, arg_type, gdef, inferred);
    }

    for (int32_t i = 0; i < num_params; i++) {
        if (inferred[i] == NULL) {
            return NULL;
        }
    }

    // Build synthetic type_args and recurse
    ASTType *synth_args = NULL;
    for (int32_t i = 0; i < num_params; i++) {
        ASTType arg = type_to_ast_type(sema->base.arena, inferred[i]);
        BUF_PUSH(synth_args, arg);
    }
    node->call.type_args = synth_args;
    return check_call(sema, node);
}
