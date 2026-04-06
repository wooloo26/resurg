#include "_sema.h"

// ── Reserved-prefix validation ─────────────────────────────────────────

/**
 * Return true (and emit a diagnostic) if @p name starts with a prefix
 * reserved for the compiler or runtime.  Checked prefixes:
 *
 *   rsg_   – runtime fns / types
 *   rsgu_  – codegen-mangled user fns
 *   _rsg   – codegen temporaries and internal vars
 *   _Rsg   – codegen compound-type names
 */
static bool is_reserved_id(Sema *analyzer, SourceLoc loc, const char *name) {
    bool reserved = strncmp(name, "rsg_", 4) == 0 || strncmp(name, "rsgu_", 5) == 0 ||
                    strncmp(name, "_rsg", 4) == 0 || strncmp(name, "_Rsg", 4) == 0;
    if (reserved) {
        SEMA_ERR(analyzer, loc, "identifier '%s' uses a prefix reserved for the compiler/runtime",
                 name);
    }
    return reserved;
}

// ── Statement / decl checkers ───────────────────────────────────

const Type *check_if(Sema *analyzer, ASTNode *node) {
    check_node(analyzer, node->if_expr.cond);
    const Type *then_type = check_node(analyzer, node->if_expr.then_body);
    const Type *else_type = NULL;
    if (node->if_expr.else_body != NULL) {
        else_type = check_node(analyzer, node->if_expr.else_body);
    }

    // Never-type coercion: if one branch is never, use the other
    if (then_type != NULL && then_type->kind == TYPE_NEVER && else_type != NULL) {
        return else_type;
    }
    if (else_type != NULL && else_type->kind == TYPE_NEVER && then_type != NULL) {
        return then_type;
    }

    // If both branches present and non-unit, return their common type
    if (else_type != NULL && then_type != NULL && then_type->kind != TYPE_UNIT) {
        return then_type;
    }
    if (else_type != NULL && else_type->kind != TYPE_UNIT) {
        return else_type;
    }
    if (then_type != NULL) {
        return then_type;
    }
    return &TYPE_UNIT_INST;
}

const Type *check_block(Sema *analyzer, ASTNode *node) {
    scope_push(analyzer, false);
    const Type *last_stmt_type = NULL;
    for (int32_t i = 0; i < BUF_LEN(node->block.stmts); i++) {
        last_stmt_type = check_node(analyzer, node->block.stmts[i]);
    }
    const Type *result_type = &TYPE_UNIT_INST;
    if (node->block.result != NULL) {
        result_type = check_node(analyzer, node->block.result);
    } else if (last_stmt_type != NULL && last_stmt_type->kind == TYPE_NEVER) {
        // Block whose last stmt diverges (return/break/continue) is never.
        result_type = &TYPE_NEVER_INST;
    }
    scope_pop(analyzer);
    return result_type;
}

/** Promote array lit elems to match the declared elem type. */
static bool promote_array_elems(ASTNode *init, const Type *declared) {
    for (int32_t i = 0; i < BUF_LEN(init->array_lit.elems); i++) {
        ASTNode *elem = init->array_lit.elems[i];
        if (promote_lit(elem, declared->array.elem) == NULL && elem->type != NULL &&
            !type_equal(elem->type, declared->array.elem)) {
            return false;
        }
    }
    return true;
}

const Type *check_var_decl(Sema *analyzer, ASTNode *node) {
    const Type *init_type = NULL;
    if (node->var_decl.init != NULL) {
        init_type = check_node(analyzer, node->var_decl.init);
    }

    const Type *declared = resolve_ast_type(analyzer, &node->var_decl.type);

    // Determine final type
    const Type *var_type;
    if (declared != NULL) {
        var_type = declared;
        // Promote lit init to match declared type
        if (init_type != NULL && node->var_decl.init != NULL) {
            promote_lit(node->var_decl.init, declared);

            // Promote array lit elems to match declared elem type
            ASTNode *init = node->var_decl.init;
            bool is_array_mismatch = init->kind == NODE_ARRAY_LIT && declared->kind == TYPE_ARRAY &&
                                     init_type->kind == TYPE_ARRAY &&
                                     !type_equal(declared, init_type);
            if (is_array_mismatch) {
                if (promote_array_elems(init, declared)) {
                    init->type = declared;
                }
            }

            // Re-read init_type after promotion
            init_type = node->var_decl.init->type;
        }
        // Check for type mismatch between declared and init (non-lit)
        if (init_type != NULL && !type_equal(declared, init_type) && init_type->kind != TYPE_ERR &&
            declared->kind != TYPE_ERR) {
            SEMA_ERR(analyzer, node->loc, "type mismatch: expected '%s', got '%s'",
                     type_name(analyzer->arena, declared), type_name(analyzer->arena, init_type));
        }
    } else if (init_type != NULL) {
        var_type = init_type;
    } else {
        SEMA_ERR(analyzer, node->loc, "cannot infer type for '%s'", node->var_decl.name);
        var_type = &TYPE_ERR_INST;
    }

    if (scope_lookup_current(analyzer, node->var_decl.name) != NULL) {
        // Same-scope rebinding is allowed (shadowing)
    }

    is_reserved_id(analyzer, node->loc, node->var_decl.name);

    scope_define(analyzer, &(SymDef){node->var_decl.name, var_type, false, SYM_VAR});

    // Mark immutable bindings
    if (node->var_decl.is_immut) {
        Sym *sym = scope_lookup_current(analyzer, node->var_decl.name);
        if (sym != NULL) {
            sym->is_immut = true;
        }
    }

    return var_type;
}

void check_fn_body(Sema *analyzer, ASTNode *fn_node) {
    is_reserved_id(analyzer, fn_node->loc, fn_node->fn_decl.name);

    scope_push(analyzer, false);

    // Register params
    for (int32_t i = 0; i < BUF_LEN(fn_node->fn_decl.params); i++) {
        ASTNode *param = fn_node->fn_decl.params[i];
        const Type *param_type = resolve_ast_type(analyzer, &param->param.type);
        if (param_type == NULL) {
            param_type = &TYPE_ERR_INST;
        }
        param->type = param_type;
        is_reserved_id(analyzer, param->loc, param->param.name);
        scope_define(analyzer, &(SymDef){param->param.name, param_type, false, SYM_PARAM});
    }

    // Check body
    if (fn_node->fn_decl.body != NULL) {
        const Type *body_type = check_node(analyzer, fn_node->fn_decl.body);

        // If return type not declared, infer from body
        const Type *resolved_return = resolve_ast_type(analyzer, &fn_node->fn_decl.return_type);
        if (resolved_return == NULL) {
            resolved_return = body_type != NULL ? body_type : &TYPE_UNIT_INST;
        }
        fn_node->type = resolved_return;

        // Update the fn's sym type to the resolved return type
        Sym *sym = scope_lookup(analyzer, fn_node->fn_decl.name);
        if (sym != NULL) {
            sym->type = resolved_return;
        }

        // Update fn signatures
        const char *sig_key = fn_node->fn_decl.name;
        if (fn_node->fn_decl.owner_struct != NULL) {
            sig_key =
                arena_sprintf(analyzer->arena, "%s.%s", fn_node->fn_decl.owner_struct, sig_key);
        }
        FnSignature *signature = sema_lookup_fn(analyzer, sig_key);
        if (signature != NULL && signature->return_type->kind == TYPE_UNIT) {
            signature->return_type = resolved_return;
        }
    }

    scope_pop(analyzer);
}

/** Shared logic for simple and compound assignment type-checking. */
static const Type *check_assignment_common(Sema *analyzer, ASTNode *target, ASTNode *value) {
    const Type *target_type = check_node(analyzer, target);
    check_node(analyzer, value);
    promote_lit(value, target_type);

    // Check immutability: cannot assign to immut bindings
    if (target->kind == NODE_ID) {
        Sym *sym = scope_lookup(analyzer, target->id.name);
        if (sym != NULL && sym->is_immut) {
            SEMA_ERR(analyzer, target->loc, "cannot assign to immutable var '%s'", target->id.name);
        }
    }

    return &TYPE_UNIT_INST;
}

const Type *check_assign(Sema *analyzer, ASTNode *node) {
    return check_assignment_common(analyzer, node->assign.target, node->assign.value);
}

const Type *check_compound_assign(Sema *analyzer, ASTNode *node) {
    return check_assignment_common(analyzer, node->compound_assign.target,
                                   node->compound_assign.value);
}

static const Type *check_loop(Sema *analyzer, ASTNode *node) {
    const Type *saved_break_type = analyzer->loop_break_type;
    analyzer->loop_break_type = NULL;
    scope_push(analyzer, true);
    check_node(analyzer, node->loop.body);
    scope_pop(analyzer);
    const Type *result = analyzer->loop_break_type;
    analyzer->loop_break_type = saved_break_type;
    return (result != NULL) ? result : &TYPE_NEVER_INST;
}

static void check_while(Sema *analyzer, ASTNode *node) {
    const Type *cond_type = check_node(analyzer, node->while_loop.cond);
    if (cond_type != NULL && cond_type->kind != TYPE_BOOL && cond_type->kind != TYPE_ERR) {
        SEMA_ERR(analyzer, node->while_loop.cond->loc, "condition must be 'bool', got '%s'",
                 type_name(analyzer->arena, cond_type));
    }
    scope_push(analyzer, true);
    check_node(analyzer, node->while_loop.body);
    scope_pop(analyzer);
}

static void check_defer(Sema *analyzer, ASTNode *node) {
    check_node(analyzer, node->defer_stmt.body);
}

static void check_for(Sema *analyzer, ASTNode *node) {
    if (node->for_loop.iterable != NULL) {
        // Slice iteration: for slice |v| or for slice |v, i|
        const Type *iter_type = check_node(analyzer, node->for_loop.iterable);
        scope_push(analyzer, true);
        const Type *elem_type = &TYPE_ERR_INST;
        if (iter_type != NULL && iter_type->kind == TYPE_SLICE) {
            elem_type = iter_type->slice.elem;
        }
        is_reserved_id(analyzer, node->loc, node->for_loop.var_name);
        scope_define(analyzer, &(SymDef){node->for_loop.var_name, elem_type, false, SYM_VAR});
        if (node->for_loop.idx_name != NULL) {
            is_reserved_id(analyzer, node->loc, node->for_loop.idx_name);
            scope_define(analyzer,
                         &(SymDef){node->for_loop.idx_name, &TYPE_I32_INST, false, SYM_VAR});
        }
        check_node(analyzer, node->for_loop.body);
        scope_pop(analyzer);
    } else {
        // Range iteration: for start..end |i|
        check_node(analyzer, node->for_loop.start);
        check_node(analyzer, node->for_loop.end);
        scope_push(analyzer, true);
        is_reserved_id(analyzer, node->loc, node->for_loop.var_name);
        scope_define(analyzer, &(SymDef){node->for_loop.var_name, &TYPE_I32_INST, false, SYM_VAR});
        check_node(analyzer, node->for_loop.body);
        scope_pop(analyzer);
    }
}

static void check_break_continue(Sema *analyzer, ASTNode *node) {
    if (!in_loop(analyzer)) {
        SEMA_ERR(analyzer, node->loc, "'%s' outside of loop",
                 node->kind == NODE_BREAK ? "break" : "continue");
    }
    if (node->kind == NODE_BREAK && node->break_stmt.value != NULL) {
        const Type *val_type = check_node(analyzer, node->break_stmt.value);
        if (analyzer->loop_break_type == NULL) {
            analyzer->loop_break_type = val_type;
        }
    }
}

// ── Struct & destructure checkers ───────────────────────────────────────

/** Type-check a single struct method: register recv + params, check body. */
static void check_struct_method_body(Sema *analyzer, ASTNode *method, const char *struct_name,
                                     const Type *struct_type) {
    scope_push(analyzer, false);

    // Register recv as a param with struct type
    if (method->fn_decl.recv_name != NULL) {
        scope_define(analyzer, &(SymDef){method->fn_decl.recv_name, struct_type, false, SYM_PARAM});
    }

    // Register method params
    for (int32_t j = 0; j < BUF_LEN(method->fn_decl.params); j++) {
        ASTNode *param = method->fn_decl.params[j];
        const Type *pt = resolve_ast_type(analyzer, &param->param.type);
        if (pt == NULL) {
            pt = &TYPE_ERR_INST;
        }
        param->type = pt;
        scope_define(analyzer, &(SymDef){param->param.name, pt, false, SYM_PARAM});
    }

    // Check body and infer return type
    if (method->fn_decl.body != NULL) {
        const Type *body_type = check_node(analyzer, method->fn_decl.body);
        const Type *return_type = resolve_ast_type(analyzer, &method->fn_decl.return_type);
        if (return_type == NULL) {
            return_type = body_type != NULL ? body_type : &TYPE_UNIT_INST;
        }
        method->type = return_type;

        // Update method signature if return type was inferred
        const char *method_key =
            arena_sprintf(analyzer->arena, "%s.%s", struct_name, method->fn_decl.name);
        FnSignature *sig = sema_lookup_fn(analyzer, method_key);
        if (sig != NULL && sig->return_type->kind == TYPE_UNIT) {
            sig->return_type = return_type;
        }
    }
    scope_pop(analyzer);
}

/** Check enum method bodies (pass 2) — delegates to check_struct_method_body. */
static const Type *check_enum_decl_body(Sema *analyzer, ASTNode *node) {
    const Type *result = node->type;
    EnumDef *edef = sema_lookup_enum(analyzer, node->enum_decl.name);
    if (edef == NULL) {
        return result;
    }
    const Type *ptr_type = type_create_ptr(analyzer->arena, edef->type, false);
    for (int32_t i = 0; i < BUF_LEN(node->enum_decl.methods); i++) {
        check_struct_method_body(analyzer, node->enum_decl.methods[i], node->enum_decl.name,
                                 ptr_type);
    }
    return result;
}

/** Pact decls are validated during pass 1; nothing to check in pass 2. */
static const Type *check_pact_decl(Sema *analyzer, ASTNode *node) {
    (void)analyzer;
    (void)node;
    return &TYPE_UNIT_INST;
}

static const Type *check_struct_decl(Sema *analyzer, ASTNode *node) {
    StructDef *sdef = sema_lookup_struct(analyzer, node->struct_decl.name);
    if (sdef == NULL) {
        return &TYPE_UNIT_INST;
    }
    const Type *result = sdef->type;

    // Check method bodies in pass 2
    for (int32_t i = 0; i < BUF_LEN(node->struct_decl.methods); i++) {
        check_struct_method_body(analyzer, node->struct_decl.methods[i], node->struct_decl.name,
                                 sdef->type);
    }

    // Check default value exprs for fields
    for (int32_t i = 0; i < BUF_LEN(node->struct_decl.fields); i++) {
        ASTStructField *f = &node->struct_decl.fields[i];
        if (f->default_value != NULL) {
            check_node(analyzer, f->default_value);
        }
    }
    return result;
}

static const Type *check_struct_destructure(Sema *analyzer, ASTNode *node) {
    const Type *value_type = check_node(analyzer, node->struct_destructure.value);
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
            StructDef *sdef = sema_lookup_struct(analyzer, value_type->struct_type.name);
            if (sdef != NULL) {
                for (int32_t j = 0; j < BUF_LEN(sdef->fields); j++) {
                    if (strcmp(sdef->fields[j].name, fname) == 0) {
                        field_type = sdef->fields[j].type;
                        break;
                    }
                }
            }
            if (field_type == NULL) {
                SEMA_ERR(analyzer, node->loc, "no field '%s' on struct '%s'", fname,
                         value_type->struct_type.name);
                field_type = &TYPE_ERR_INST;
            }
            scope_define(analyzer, &(SymDef){var_name, field_type, false, SYM_VAR});
        }
    } else if (value_type != NULL && value_type->kind != TYPE_ERR) {
        SEMA_ERR(analyzer, node->loc, "struct destructuring requires a struct type");
    }
    return &TYPE_UNIT_INST;
}

static const Type *check_tuple_destructure(Sema *analyzer, ASTNode *node) {
    const Type *value_type = check_node(analyzer, node->tuple_destructure.value);
    if (value_type != NULL && value_type->kind == TYPE_TUPLE) {
        int32_t name_count = BUF_LEN(node->tuple_destructure.names);
        int32_t tuple_count = value_type->tuple.count;
        bool has_rest = node->tuple_destructure.has_rest;

        if (has_rest) {
            if (name_count > tuple_count) {
                SEMA_ERR(analyzer, node->loc,
                         "too many names in tuple destructure with '..': "
                         "tuple has %d elems, got %d names",
                         tuple_count, name_count);
            }
        } else {
            if (name_count != tuple_count) {
                SEMA_ERR(analyzer, node->loc,
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
            scope_define(analyzer,
                         &(SymDef){vname, value_type->tuple.elems[elem_idx], false, SYM_VAR});
        }
    } else if (value_type != NULL && value_type->kind != TYPE_ERR) {
        SEMA_ERR(analyzer, node->loc, "tuple destructuring requires a tuple type");
    }
    return &TYPE_UNIT_INST;
}

// ── Node dispatch ──────────────────────────────────────────────────────

const Type *check_node(Sema *analyzer, ASTNode *node) {
    if (node == NULL) {
        return &TYPE_UNIT_INST;
    }
    const Type *result = &TYPE_UNIT_INST;

    switch (node->kind) {
    case NODE_FILE:
        for (int32_t i = 0; i < BUF_LEN(node->file.decls); i++) {
            check_node(analyzer, node->file.decls[i]);
        }
        break;

    case NODE_MODULE:
        analyzer->current_scope->module_name = node->module.name;
        break;

    case NODE_TYPE_ALIAS:
        // Already processed in first pass
        break;

    case NODE_FN_DECL:
        check_fn_body(analyzer, node);
        result = node->type; // preserve type set by check_fn_body
        break;

    case NODE_VAR_DECL:
        result = check_var_decl(analyzer, node);
        break;

    case NODE_PARAM:
        result = resolve_ast_type(analyzer, &node->param.type);
        if (result == NULL) {
            result = &TYPE_ERR_INST;
        }
        break;

    case NODE_EXPR_STMT:
        check_node(analyzer, node->expr_stmt.expr);
        break;

    case NODE_BREAK:
    case NODE_CONTINUE:
        check_break_continue(analyzer, node);
        result = &TYPE_NEVER_INST;
        break;

    case NODE_LIT:
        result = check_lit(analyzer, node);
        break;

    case NODE_ID:
        result = check_id(analyzer, node);
        break;

    case NODE_UNARY:
        result = check_unary(analyzer, node);
        break;

    case NODE_BINARY:
        result = check_binary(analyzer, node);
        break;

    case NODE_ASSIGN:
        result = check_assign(analyzer, node);
        break;

    case NODE_COMPOUND_ASSIGN:
        result = check_compound_assign(analyzer, node);
        break;

    case NODE_CALL:
        result = check_call(analyzer, node);
        break;

    case NODE_MEMBER:
        result = check_member(analyzer, node);
        break;

    case NODE_IDX:
        result = check_idx(analyzer, node);
        break;

    case NODE_IF:
        result = check_if(analyzer, node);
        break;

    case NODE_LOOP:
        result = check_loop(analyzer, node);
        break;

    case NODE_FOR:
        check_for(analyzer, node);
        break;

    case NODE_BLOCK:
        result = check_block(analyzer, node);
        break;

    case NODE_STR_INTERPOLATION:
        result = check_str_interpolation(analyzer, node);
        break;

    case NODE_ARRAY_LIT:
        result = check_array_lit(analyzer, node);
        break;

    case NODE_SLICE_LIT:
        result = check_slice_lit(analyzer, node);
        break;

    case NODE_SLICE_EXPR:
        result = check_slice_expr(analyzer, node);
        break;

    case NODE_TUPLE_LIT:
        result = check_tuple_lit(analyzer, node);
        break;

    case NODE_TYPE_CONVERSION:
        result = check_type_conversion(analyzer, node);
        break;

    case NODE_STRUCT_DECL:
        result = check_struct_decl(analyzer, node);
        break;

    case NODE_PACT_DECL:
        result = check_pact_decl(analyzer, node);
        break;

    case NODE_STRUCT_LIT:
        result = check_struct_lit(analyzer, node);
        break;

    case NODE_STRUCT_DESTRUCTURE:
        result = check_struct_destructure(analyzer, node);
        break;

    case NODE_TUPLE_DESTRUCTURE:
        result = check_tuple_destructure(analyzer, node);
        break;

    case NODE_ADDRESS_OF:
        result = check_address_of(analyzer, node);
        break;

    case NODE_DEREF:
        result = check_deref(analyzer, node);
        break;

    case NODE_ENUM_DECL:
        result = check_enum_decl_body(analyzer, node);
        break;

    case NODE_RETURN:
        if (node->return_stmt.value != NULL) {
            check_node(analyzer, node->return_stmt.value);
        }
        result = &TYPE_NEVER_INST;
        break;

    case NODE_WHILE:
        check_while(analyzer, node);
        break;

    case NODE_DEFER:
        check_defer(analyzer, node);
        break;

    case NODE_MATCH:
        result = check_match(analyzer, node);
        break;

    case NODE_ENUM_INIT:
        result = check_enum_init(analyzer, node);
        break;
    }

    node->type = result;
    return result;
}
