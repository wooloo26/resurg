#include "_check.h"

/**
 * @file check_method.c
 * @brief Method call dispatch — struct methods, module-qualified calls, closures.
 */

// ── Struct method resolution ──────────────────────────────────────

/** Resolve a struct method call, including promoted methods from embedded structs. */
static const Type *check_struct_method_call(Sema *sema, ASTNode *node, const Type *struct_type,
                                            const char *method_name) {
    const char *method_key =
        arena_sprintf(sema->base.arena, "%s.%s", struct_type->struct_type.name, method_name);
    FnSig *sig = sema_lookup_fn(sema, method_key);

    // If not found directly, check embedded structs for promoted methods
    if (sig == NULL) {
        StructDef *sdef = sema_lookup_struct(sema, struct_type->struct_type.name);
        if (sdef != NULL) {
            for (int32_t ei = 0; ei < BUF_LEN(sdef->embedded); ei++) {
                const char *embed_key =
                    arena_sprintf(sema->base.arena, "%s.%s", sdef->embedded[ei], method_name);
                sig = sema_lookup_fn(sema, embed_key);
                if (sig != NULL) {
                    break;
                }
            }
        }
    }
    if (sig == NULL) {
        return NULL;
    }
    // Reject pointer receiver on rvalue (literal, call result, etc.)
    if (sig->is_ptr_recv && !is_lvalue(node->call.callee->member.object)) {
        SEMA_ERR(sema, node->loc, "cannot call pointer receiver on rvalue");
        return &TYPE_ERR_INST;
    }
    for (int32_t i = 0; i < BUF_LEN(node->call.args); i++) {
        check_node(sema, node->call.args[i]);
    }
    return resolve_call(sema, node, sig);
}

// ── Module-qualified calls ────────────────────────────────────────

/** Output from module-qualified call resolution. */
typedef struct {
    const Type *type;     // non-NULL → direct resolve done; TYPE_ERR on error
    const char *fn_name;  // updated fn_name (may be same as method_name)
    const Type *obj_type; // updated obj_type (may differ for sub-modules/enums)
} ModuleCallResult;

/**
 * Handle module-qualified calls: mod::fn(args), mod::Enum::Variant(args),
 * mod::Type, or mod::inner sub-module lookup.
 *
 * @return Result with .type == NULL when the caller should continue dispatch.
 */
static ModuleCallResult try_module_qualified_call(Sema *sema, ASTNode *node, const Type *obj_type,
                                                  const char *method_name) {
    ModuleCallResult result = {.type = NULL, .fn_name = method_name, .obj_type = obj_type};
    const char *mod_name = obj_type->module_type.name;
    const char *qualified;
    if (strlen(mod_name) == 0) {
        qualified = method_name;
    } else {
        qualified = arena_sprintf(sema->base.arena, "%s.%s", mod_name, method_name);
    }

    // Try as fn call: mod::fn(args)
    FnSig *sig = sema_lookup_fn(sema, qualified);
    if (sig != NULL) {
        if (!sig->is_pub) {
            SEMA_ERR(sema, node->loc, "'%s' is private in module '%s'", method_name, mod_name);
            result.type = &TYPE_ERR_INST;
            return result;
        }
        node->call.callee->kind = NODE_ID;
        node->call.callee->id.name = qualified;
        for (int32_t i = 0; i < BUF_LEN(node->call.args); i++) {
            check_node(sema, node->call.args[i]);
        }
        result.type = resolve_call(sema, node, sig);
        return result;
    }

    // Try as struct lit or enum access: mod::Type
    StructDef *sdef = sema_lookup_struct(sema, qualified);
    if (sdef != NULL) {
        node->call.callee->member.object->type = sdef->type;
        result.fn_name = method_name;
        return result;
    }

    EnumDef *edef = sema_lookup_enum(sema, qualified);
    if (edef != NULL) {
        node->call.callee->member.object->type = edef->type;
        node->call.callee->member.object->id.name = qualified;
        result.obj_type = edef->type;
        return result;
    }

    // Try as sub-module: mod::inner::...
    Sym *mod_sym = scope_lookup(sema, qualified);
    if (mod_sym != NULL && mod_sym->type != NULL && mod_sym->type->kind == TYPE_MODULE) {
        result.obj_type = mod_sym->type;
    }
    return result;
}

// ── Member call dispatch ──────────────────────────────────────────

const Type *check_member_call(Sema *sema, ASTNode *node, const char **out_fn_name) {
    const char *method_name = node->call.callee->member.member;
    const Type *obj_type = NULL;

    // Handle generic enum instantiation via type_args on the call node.
    // Check BEFORE calling check_node to avoid spurious "undefined variable" errors.
    if (node->call.callee->member.object->kind == NODE_ID && BUF_LEN(node->call.type_args) > 0) {
        const char *enum_name = node->call.callee->member.object->id.name;
        GenericEnumDef *gdef = sema_lookup_generic_enum(sema, enum_name);
        if (gdef != NULL) {
            GenericInstArgs inst_args = {node->call.type_args, BUF_LEN(node->call.type_args),
                                         node->loc};
            const char *mangled = instantiate_generic_enum(sema, gdef, &inst_args);
            if (mangled != NULL) {
                node->call.callee->member.object->id.name = mangled;
                EnumDef *edef = sema_lookup_enum(sema, mangled);
                obj_type = edef->type;
                node->call.callee->member.object->type = obj_type;
                // Clear type_args since they've been consumed for enum instantiation
                node->call.type_args = NULL;
            }
        }
    }

    if (obj_type == NULL) {
        obj_type = check_node(sema, node->call.callee->member.object);
    }

    // Auto-deref for ptr types
    if (obj_type != NULL && obj_type->kind == TYPE_PTR) {
        obj_type = obj_type->ptr.pointee;
    }

    // Module-qualified call: mod::fn(args) or mod::Enum::Variant(args)
    if (obj_type != NULL && obj_type->kind == TYPE_MODULE) {
        ModuleCallResult mod = try_module_qualified_call(sema, node, obj_type, method_name);
        if (mod.type != NULL) {
            return mod.type;
        }
        *out_fn_name = mod.fn_name;
        obj_type = mod.obj_type;
    }

    if (obj_type != NULL && obj_type->kind == TYPE_ENUM) {
        const Type *result = check_enum_variant_call(sema, node, obj_type, method_name);
        if (result != NULL) {
            return result;
        }
        // Enum method call
        const char *method_key =
            arena_sprintf(sema->base.arena, "%s.%s", type_enum_name(obj_type), method_name);
        FnSig *sig = sema_lookup_fn(sema, method_key);
        if (sig != NULL) {
            // Reject pointer receiver on rvalue
            if (sig->is_ptr_recv && !is_lvalue(node->call.callee->member.object)) {
                SEMA_ERR(sema, node->loc, "cannot call pointer receiver on rvalue");
                return &TYPE_ERR_INST;
            }
            return resolve_call(sema, node, sig);
        }
    }

    if (obj_type != NULL && obj_type->kind == TYPE_STRUCT) {
        const Type *result = check_struct_method_call(sema, node, obj_type, method_name);
        if (result != NULL) {
            return result;
        }
    }

    // Call through fn-typed struct field: obj.field(args)
    if (obj_type != NULL && obj_type->kind == TYPE_STRUCT) {
        const StructField *sf = type_struct_find_field(obj_type, method_name);
        if (sf != NULL && sf->type != NULL && sf->type->kind == TYPE_FN) {
            node->call.callee->type = sf->type;
            return check_fn_type_call(sema, node, sf->type);
        }
    }

    // Extension method call on primitive and compound types
    if (obj_type != NULL) {
        const char *prim_name = type_name(sema->base.arena, obj_type);
        if (prim_name != NULL) {
            const char *method_key =
                arena_sprintf(sema->base.arena, "%s.%s", prim_name, method_name);
            FnSig *sig = sema_lookup_fn(sema, method_key);

            // Generic ext instantiation: ext<T> []T or ext<T, comptime N: usize> [N]T
            if (sig == NULL && obj_type->kind == TYPE_SLICE) {
                sig = instantiate_compound_ext(sema, "[]", obj_type, prim_name, method_name);
            }
            if (sig == NULL && obj_type->kind == TYPE_ARRAY) {
                sig = instantiate_compound_ext(sema, "[_]", obj_type, prim_name, method_name);
            }

            if (sig != NULL) {
                // Reject pointer receiver on rvalue (literal, call result, etc.)
                if (sig->is_ptr_recv && !is_lvalue(node->call.callee->member.object)) {
                    SEMA_ERR(sema, node->loc, "cannot call pointer receiver on rvalue");
                    return &TYPE_ERR_INST;
                }
                for (int32_t i = 0; i < BUF_LEN(node->call.args); i++) {
                    check_node(sema, node->call.args[i]);
                }
                return resolve_call(sema, node, sig);
            }
        }
    }

    *out_fn_name = method_name;
    return NULL;
}

// ── Inline closure call ───────────────────────────────────────────

const Type *check_inline_closure_call(Sema *sema, ASTNode *node) {
    for (int32_t i = 0; i < BUF_LEN(node->call.args); i++) {
        check_node(sema, node->call.args[i]);
    }
    int32_t param_count = BUF_LEN(node->call.callee->closure.params);
    int32_t arg_count = BUF_LEN(node->call.args);
    const Type **param_types = NULL;
    for (int32_t i = 0; i < param_count; i++) {
        const Type *pt = (i < arg_count && node->call.args[i]->type != NULL)
                             ? node->call.args[i]->type
                             : &TYPE_ERR_INST;
        BUF_PUSH(param_types, pt);
    }
    FnTypeSpec fn_spec = {param_types, param_count, NULL, FN_PLAIN};
    const Type *expected_fn = type_create_fn(sema->base.arena, &fn_spec);
    SEMA_INFER_SCOPE(sema, expected_type, expected_fn);
    const Type *callee_type = check_closure(sema, node->call.callee);
    SEMA_INFER_RESTORE(sema, expected_type);
    node->call.callee->type = callee_type;
    if (callee_type != NULL && callee_type->kind == TYPE_FN) {
        return callee_type->fn_type.return_type;
    }
    return &TYPE_ERR_INST;
}
