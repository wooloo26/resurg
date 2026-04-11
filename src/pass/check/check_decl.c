#include "_check.h"

/**
 * @file check_decl.c
 * @brief Type-checking for struct/enum declarations, method bodies, and
 *        destructuring patterns (struct and tuple).
 */

// ── Method body checking ────────────────────────────────────────────────

/** Type-check a single struct method: register recv + params, check body. */
void check_struct_method_body(Sema *sema, ASTNode *method, const char *struct_name,
                              const Type *struct_type) {
    SEMA_INFER_SCOPE(sema, self_type_name, struct_name);
    scope_push(sema, false);

    // Register recv as a param with struct type
    if (method->fn_decl.recv_name != NULL) {
        const Type *recv_reg_type = struct_type;
        if (method->fn_decl.is_ptr_recv && struct_type->kind != TYPE_PTR) {
            recv_reg_type =
                type_create_ptr(sema->base.arena, struct_type, method->fn_decl.is_mut_recv);
        } else if (!method->fn_decl.is_ptr_recv && struct_type->kind == TYPE_PTR) {
            // Value recv on pointer-wrapped type (e.g. enum ext): unwrap.
            recv_reg_type = struct_type->ptr.pointee;
        }
        scope_define(sema, &(SymDef){method->fn_decl.recv_name, recv_reg_type, false, SYM_PARAM});
    }

    // Register method params
    for (int32_t j = 0; j < BUF_LEN(method->fn_decl.params); j++) {
        ASTNode *param = method->fn_decl.params[j];
        const Type *pt = resolve_ast_type(sema, &param->param.type);
        if (pt == NULL) {
            pt = &TYPE_ERR_INST;
        }
        // Variadic param: ..T → []T (slice type)
        if (param->param.is_variadic) {
            pt = type_create_slice(sema->base.arena, pt);
        }
        param->type = pt;
        scope_define(sema, &(SymDef){param->param.name, pt, false, SYM_PARAM});
    }

    // Check body and infer return type
    if (method->fn_decl.body != NULL) {
        // Pre-resolve return type for bidirectional inference (Ok/Err/None)
        const Type *pre_return = resolve_ast_type(sema, &method->fn_decl.return_type);
        SEMA_INFER_SCOPE(sema, fn_return_type,
                         pre_return != NULL ? pre_return : sema->infer.fn_return_type);
        SEMA_INFER_SCOPE(sema, expected_type,
                         pre_return != NULL ? pre_return : sema->infer.expected_type);

        const Type *body_type = check_node(sema, method->fn_decl.body);

        SEMA_INFER_RESTORE(sema, expected_type);
        SEMA_INFER_RESTORE(sema, fn_return_type);

        const Type *return_type = pre_return;
        if (return_type == NULL) {
            return_type = body_type != NULL ? body_type : &TYPE_UNIT_INST;
        }
        method->type = return_type;

        // Update method sig if return type was inferred
        const char *method_key =
            arena_sprintf(sema->base.arena, "%s.%s", struct_name, method->fn_decl.name);
        FnSig *sig = sema_lookup_fn(sema, method_key);
        if (sig != NULL && sig->return_type->kind == TYPE_UNIT) {
            sig->return_type = return_type;
        }
    }
    scope_pop(sema);
    SEMA_INFER_RESTORE(sema, self_type_name);
}

// ── Decl-level checkers ─────────────────────────────────────────────────

const Type *check_enum_decl_body(Sema *sema, ASTNode *node) {
    const Type *result = node->type;
    EnumDef *edef = sema_lookup_enum(sema, node->enum_decl->name);
    if (edef == NULL) {
        return result;
    }
    const Type *ptr_type = type_create_ptr(sema->base.arena, edef->type, false);
    for (int32_t i = 0; i < BUF_LEN(node->enum_decl->methods); i++) {
        check_struct_method_body(sema, node->enum_decl->methods[i], node->enum_decl->name,
                                 ptr_type);
    }
    return result;
}

const Type *check_pact_decl(Sema *sema, ASTNode *node) {
    (void)sema;
    (void)node;
    return &TYPE_UNIT_INST;
}

const Type *check_struct_decl(Sema *sema, ASTNode *node) {
    StructDef *sdef = sema_lookup_struct(sema, node->struct_decl->name);
    if (sdef == NULL) {
        return &TYPE_UNIT_INST;
    }
    const Type *result = sdef->type;

    // Check method bodies in pass 2
    for (int32_t i = 0; i < BUF_LEN(node->struct_decl->methods); i++) {
        check_struct_method_body(sema, node->struct_decl->methods[i], node->struct_decl->name,
                                 sdef->type);
    }

    // Check default value exprs for fields
    for (int32_t i = 0; i < BUF_LEN(node->struct_decl->fields); i++) {
        ASTStructField *f = &node->struct_decl->fields[i];
        if (f->default_value != NULL) {
            const Type *field_type = resolve_ast_type(sema, &f->type);
            SEMA_INFER_SCOPE(sema, expected_type, field_type);
            check_node(sema, f->default_value);
            SEMA_INFER_RESTORE(sema, expected_type);
        }
    }
    return result;
}

// ── Ext decl checking ───────────────────────────────────────────────────

const Type *check_ext_decl(Sema *sema, ASTNode *node) {
    const char *target_name = node->ext_decl->target_name;

    // Skip generic ext templates (checked during monomorphization)
    if (BUF_LEN(node->ext_decl->type_params) > 0) {
        return &TYPE_UNIT_INST;
    }

    // Determine receiver type for method body checking
    const Type *recv_type = NULL;
    const Type *check_recv_type = NULL;

    StructDef *sdef = sema_lookup_struct(sema, target_name);
    if (sdef != NULL) {
        recv_type = sdef->type;
        check_recv_type = sdef->type;
    }
    EnumDef *edef = (recv_type == NULL) ? sema_lookup_enum(sema, target_name) : NULL;
    if (edef != NULL) {
        recv_type = edef->type; // raw TYPE_ENUM for lower pass
        check_recv_type = type_create_ptr(sema->base.arena, edef->type, false); // ptr for scope
    }
    if (recv_type == NULL) {
        // Primitive type — look up the singleton instance
        recv_type = type_from_name(target_name);
        check_recv_type = recv_type;
    }
    if (recv_type == NULL) {
        return &TYPE_UNIT_INST;
    }

    for (int32_t i = 0; i < BUF_LEN(node->ext_decl->methods); i++) {
        check_struct_method_body(sema, node->ext_decl->methods[i], target_name, check_recv_type);
    }

    node->type = recv_type;
    return &TYPE_UNIT_INST;
}

// ── Destructure checkers ────────────────────────────────────────────────

const Type *check_struct_destructure(Sema *sema, ASTNode *node) {
    const Type *value_type = check_node(sema, node->struct_destructure.value);
    if (value_type != NULL && value_type->kind == TYPE_STRUCT) {
        for (int32_t i = 0; i < BUF_LEN(node->struct_destructure.field_names); i++) {
            const char *fname = node->struct_destructure.field_names[i];
            const char *alias = (node->struct_destructure.aliases != NULL &&
                                 i < BUF_LEN(node->struct_destructure.aliases))
                                    ? node->struct_destructure.aliases[i]
                                    : NULL;
            const char *var_name = (alias != NULL) ? alias : fname;

            // Look up field in struct def (includes promoted fields)
            const Type *field_type = NULL;
            StructDef *sdef = sema_lookup_struct(sema, value_type->struct_type.name);
            if (sdef != NULL) {
                for (int32_t j = 0; j < BUF_LEN(sdef->fields); j++) {
                    if (strcmp(sdef->fields[j].name, fname) == 0) {
                        field_type = sdef->fields[j].type;
                        break;
                    }
                }
            }
            if (field_type == NULL) {
                SEMA_ERR(sema, node->loc, "no field '%s' on struct '%s'", fname,
                         value_type->struct_type.name);
                field_type = &TYPE_ERR_INST;
            }
            scope_define(sema, &(SymDef){var_name, field_type, false, SYM_VAR});
        }
    } else if (value_type != NULL && value_type->kind != TYPE_ERR) {
        SEMA_ERR(sema, node->loc, "struct destructuring requires a struct type");
    }
    return &TYPE_UNIT_INST;
}

const Type *check_tuple_destructure(Sema *sema, ASTNode *node) {
    const Type *value_type = check_node(sema, node->tuple_destructure.value);
    if (value_type != NULL && value_type->kind == TYPE_TUPLE) {
        int32_t name_count = BUF_LEN(node->tuple_destructure.names);
        int32_t tuple_count = value_type->tuple.count;
        bool has_rest = node->tuple_destructure.has_rest;

        if (has_rest) {
            if (name_count > tuple_count) {
                SEMA_ERR(sema, node->loc,
                         "too many names in tuple destructure with '..': "
                         "tuple has %d elems, got %d names",
                         tuple_count, name_count);
            }
        } else {
            if (name_count != tuple_count) {
                SEMA_ERR(sema, node->loc,
                         "tuple destructure requires %d names but got %d"
                         " (use '..' to ignore elems)",
                         tuple_count, name_count);
            }
        }

        int32_t rest_pos = node->tuple_destructure.rest_pos;
        int32_t skipped = has_rest ? (tuple_count - name_count) : 0;

        for (int32_t i = 0; i < name_count && i < tuple_count; i++) {
            const char *vname = node->tuple_destructure.names[i];
            int32_t elem_idx = i;
            if (has_rest && i >= rest_pos) {
                elem_idx = i + skipped;
            }
            if (elem_idx >= tuple_count) {
                break;
            }
            if (vname[0] == '_') {
                continue;
            }
            scope_define(sema, &(SymDef){vname, value_type->tuple.elems[elem_idx], false, SYM_VAR});
        }
    } else if (value_type != NULL && value_type->kind != TYPE_ERR) {
        SEMA_ERR(sema, node->loc, "tuple destructuring requires a tuple type");
    }
    return &TYPE_UNIT_INST;
}
