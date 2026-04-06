#include "_lower.h"

// ── Statement / control-flow lower ─────────────────────────────────

HirNode *lower_stmt_if(Lower *low, const ASTNode *ast) {
    HirNode *cond = lower_expr(low, ast->if_expr.cond);
    HirNode *then_body = lower_node(low, ast->if_expr.then_body);
    HirNode *else_body = NULL;
    if (ast->if_expr.else_body != NULL) {
        else_body = lower_node(low, ast->if_expr.else_body);
    }
    HirNode *node = hir_new(low->hir_arena, HIR_IF, ast->type, ast->loc);
    node->if_expr.cond = cond;
    node->if_expr.then_body = then_body;
    node->if_expr.else_body = else_body;
    return node;
}

static HirNode *lower_var_decl(Lower *low, const ASTNode *ast) {
    const char *name = ast->var_decl.name;
    const Type *type = ast->type != NULL ? ast->type : &TYPE_ERR_INST;
    bool is_mut = ast->var_decl.is_var;

    HirSymSpec var_spec = {HIR_SYM_VAR, name, type, is_mut, ast->loc};
    HirSym *sym = lower_add_var(low, &var_spec);

    HirNode *init = NULL;
    if (ast->var_decl.init != NULL) {
        init = lower_expr(low, ast->var_decl.init);
    }

    return lower_make_var_decl(low, sym, init);
}

static HirNode *lower_assign(Lower *low, const ASTNode *ast) {
    HirNode *target = lower_expr(low, ast->assign.target);
    HirNode *value = lower_expr(low, ast->assign.value);
    HirNode *node = hir_new(low->hir_arena, HIR_ASSIGN, &TYPE_UNIT_INST, ast->loc);
    node->assign.target = target;
    node->assign.value = value;
    return node;
}

/** Desugar `x op= expr` → `x = x op expr`. */
static HirNode *lower_compound_assign(Lower *low, const ASTNode *ast) {
    HirNode *target = lower_expr(low, ast->compound_assign.target);
    HirNode *value = lower_expr(low, ast->compound_assign.value);

    // Build binary: target op value
    TokenKind base_op = lower_compound_to_base_op(ast->compound_assign.op);
    HirNode *target_read = lower_expr(low, ast->compound_assign.target);
    HirNode *binary = hir_new(low->hir_arena, HIR_BINARY, target->type, ast->loc);
    binary->binary.op = base_op;
    binary->binary.left = target_read;
    binary->binary.right = value;

    // Assign: target = binary
    HirNode *node = hir_new(low->hir_arena, HIR_ASSIGN, &TYPE_UNIT_INST, ast->loc);
    node->assign.target = target;
    node->assign.value = binary;
    return node;
}

/** Expand a struct destructure into var decls in @p stmts. */
static void lower_struct_destructure_into(Lower *low, const ASTNode *ast, HirNode ***stmts) {
    HirNode *value = lower_expr(low, ast->struct_destructure.value);
    const Type *struct_type = value->type;

    const char *tmp_name = lower_make_temp_name(low);
    HirSymSpec tmp_spec = {HIR_SYM_VAR, tmp_name, struct_type, false, ast->loc};
    HirSym *tmp_sym = lower_add_var(low, &tmp_spec);
    BUF_PUSH(*stmts, lower_make_var_decl(low, tmp_sym, value));

    for (int32_t i = 0; i < BUF_LEN(ast->struct_destructure.field_names); i++) {
        const char *fname = ast->struct_destructure.field_names[i];
        const char *alias = (ast->struct_destructure.aliases != NULL &&
                             i < BUF_LEN(ast->struct_destructure.aliases))
                                ? ast->struct_destructure.aliases[i]
                                : NULL;
        const char *var_name = (alias != NULL) ? alias : fname;
        const Type *field_type = NULL;
        HirNode *field_access = NULL;

        // Check direct fields
        const StructField *sf = type_struct_find_field(struct_type, fname);
        if (sf != NULL) {
            field_type = sf->type;
            field_access = hir_new(low->hir_arena, HIR_STRUCT_FIELD_ACCESS, field_type, ast->loc);
            field_access->struct_field_access.object = lower_make_var_ref(low, tmp_sym, ast->loc);
            field_access->struct_field_access.field = fname;
            field_access->struct_field_access.via_ptr = false;
        } else {
            // Check promoted fields from embedded structs
            HirNode *tmp_ref = lower_make_var_ref(low, tmp_sym, ast->loc);
            FieldLookup lookup = {tmp_ref, struct_type, fname, false, ast->loc};
            field_access = lower_resolve_promoted_field(low, &lookup);
            if (field_access != NULL) {
                field_type = field_access->type;
            }
        }

        if (field_type == NULL) {
            field_type = &TYPE_ERR_INST;
        }

        HirSymSpec vspec = {HIR_SYM_VAR, var_name, field_type, false, ast->loc};
        HirSym *var_sym = lower_add_var(low, &vspec);
        BUF_PUSH(*stmts, lower_make_var_decl(low, var_sym, field_access));
    }
}

/** Expand a tuple destructure into var decls in @p stmts. */
static void lower_tuple_destructure_into(Lower *low, const ASTNode *ast, HirNode ***stmts) {
    HirNode *value = lower_expr(low, ast->tuple_destructure.value);
    const Type *tuple_type = value->type;

    const char *tmp_name = lower_make_temp_name(low);
    HirSymSpec tmp_spec2 = {HIR_SYM_VAR, tmp_name, tuple_type, false, ast->loc};
    HirSym *tmp_sym = lower_add_var(low, &tmp_spec2);
    BUF_PUSH(*stmts, lower_make_var_decl(low, tmp_sym, value));

    int32_t name_count = BUF_LEN(ast->tuple_destructure.names);
    bool has_rest = ast->tuple_destructure.has_rest;
    int32_t rest_pos = ast->tuple_destructure.rest_pos;
    int32_t tuple_count =
        (tuple_type != NULL && tuple_type->kind == TYPE_TUPLE) ? tuple_type->tuple.count : 0;
    int32_t skipped = has_rest ? (tuple_count - name_count) : 0;

    for (int32_t i = 0; i < name_count; i++) {
        const char *vname = ast->tuple_destructure.names[i];

        // Compute the elem idx, accounting for `..`
        int32_t elem_idx = i;
        if (has_rest && i >= rest_pos) {
            elem_idx = i + skipped;
        }

        const Type *elem_type = &TYPE_ERR_INST;
        if (tuple_type != NULL && tuple_type->kind == TYPE_TUPLE && elem_idx < tuple_count) {
            elem_type = tuple_type->tuple.elems[elem_idx];
        }

        // Skip var creation for `_` or `_`-prefixed names
        if (vname[0] == '_') {
            continue;
        }

        HirNode *idx_access = hir_new(low->hir_arena, HIR_TUPLE_IDX, elem_type, ast->loc);
        idx_access->tuple_idx.object = lower_make_var_ref(low, tmp_sym, ast->loc);
        idx_access->tuple_idx.elem_idx = elem_idx;

        HirSymSpec vspec2 = {HIR_SYM_VAR, vname, elem_type, false, ast->loc};
        HirSym *var_sym = lower_add_var(low, &vspec2);
        BUF_PUSH(*stmts, lower_make_var_decl(low, var_sym, idx_access));
    }
}

HirNode *lower_block(Lower *low, const ASTNode *ast) {
    if (ast == NULL) {
        return NULL;
    }
    assert(ast->kind == NODE_BLOCK);
    lower_scope_enter(low);

    HirNode **stmts = NULL;
    for (int32_t i = 0; i < BUF_LEN(ast->block.stmts); i++) {
        const ASTNode *stmt = ast->block.stmts[i];
        if (stmt->kind == NODE_STRUCT_DESTRUCTURE) {
            lower_struct_destructure_into(low, stmt, &stmts);
        } else if (stmt->kind == NODE_TUPLE_DESTRUCTURE) {
            lower_tuple_destructure_into(low, stmt, &stmts);
        } else {
            HirNode *s = lower_node(low, stmt);
            if (s != NULL) {
                BUF_PUSH(stmts, s);
            }
        }
    }

    HirNode *result = NULL;
    if (ast->block.result != NULL) {
        // Assignments in result pos are side-effect-only stmts.
        if (ast->block.result->kind == NODE_ASSIGN) {
            HirNode *stmt = lower_assign(low, ast->block.result);
            BUF_PUSH(stmts, stmt);
        } else if (ast->block.result->kind == NODE_COMPOUND_ASSIGN) {
            HirNode *stmt = lower_compound_assign(low, ast->block.result);
            BUF_PUSH(stmts, stmt);
        } else {
            result = lower_expr(low, ast->block.result);
        }
    }

    lower_scope_leave(low);

    HirNode *node = hir_new(low->hir_arena, HIR_BLOCK,
                            ast->type != NULL ? ast->type : &TYPE_UNIT_INST, ast->loc);
    node->block.stmts = stmts;
    node->block.result = result;
    return node;
}

static HirNode *lower_defer(Lower *low, const ASTNode *ast) {
    HirNode *body = lower_node(low, ast->defer_stmt.body);
    HirNode *node = hir_new(low->hir_arena, HIR_DEFER, &TYPE_UNIT_INST, ast->loc);
    node->defer_stmt.body = body;
    return node;
}

static HirNode *lower_expr_stmt(Lower *low, const ASTNode *ast) {
    const ASTNode *inner = ast->expr_stmt.expr;

    // Assignments are stmts in HIR — unwrap and lower directly.
    if (inner->kind == NODE_ASSIGN) {
        return lower_assign(low, inner);
    }
    if (inner->kind == NODE_COMPOUND_ASSIGN) {
        return lower_compound_assign(low, inner);
    }

    return lower_expr(low, inner);
}

/** Pre-register method syms for a named type (struct or enum). */
static void preregister_type_methods(Lower *low, const char *type_name, ASTNode *const *methods) {
    for (int32_t j = 0; j < BUF_LEN(methods); j++) {
        const ASTNode *method = methods[j];
        const char *method_name = method->fn_decl.name;
        const Type *ret = method->type != NULL ? method->type : &TYPE_UNIT_INST;
        const char *key = arena_sprintf(low->hir_arena, "%s.%s", type_name, method_name);
        const char *mangled = arena_sprintf(low->hir_arena, "rsgu_%s_%s", type_name, method_name);
        HirSymSpec sym_spec = {HIR_SYM_FN, key, ret, false, method->loc};
        HirSym *sym = lower_make_sym(low, &sym_spec);
        sym->mangled_name = mangled;
        lower_scope_define(low, key, sym);
    }
}

/** Pre-register all fn decls into scope before lower bodies. */
static void preregister_fns(Lower *low, const ASTNode *file_ast) {
    for (int32_t i = 0; i < BUF_LEN(file_ast->file.decls); i++) {
        const ASTNode *decl = file_ast->file.decls[i];
        if (decl->kind == NODE_FN_DECL) {
            const Type *ret = decl->type != NULL ? decl->type : &TYPE_UNIT_INST;
            HirSymSpec fn_spec = {HIR_SYM_FN, decl->fn_decl.name, ret, false, decl->loc};
            HirSym *sym = lower_make_sym(low, &fn_spec);
            sym->mangled_name = arena_sprintf(low->hir_arena, "rsgu_%s", decl->fn_decl.name);
            lower_scope_define(low, decl->fn_decl.name, sym);
        }
        if (decl->kind == NODE_STRUCT_DECL) {
            preregister_type_methods(low, decl->struct_decl.name, decl->struct_decl.methods);
        }
        if (decl->kind == NODE_ENUM_DECL) {
            preregister_type_methods(low, decl->enum_decl.name, decl->enum_decl.methods);
        }
    }
}

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
    struct_decl->struct_decl.name = decl_ast->struct_decl.name;
    struct_decl->struct_decl.struct_type = decl_ast->type;
    BUF_PUSH(*decls, struct_decl);

    lower_methods_into(low, decl_ast->struct_decl.methods, decl_ast->struct_decl.name,
                       decl_ast->type, decls);
}

/** Emit an enum type decl and its methods into @p decls. */
static void emit_enum_with_methods(Lower *low, const ASTNode *decl_ast, HirNode ***decls) {
    HirNode *enum_decl = hir_new(low->hir_arena, HIR_ENUM_DECL, &TYPE_UNIT_INST, decl_ast->loc);
    enum_decl->enum_decl.name = decl_ast->enum_decl.name;
    enum_decl->enum_decl.enum_type = decl_ast->type;
    BUF_PUSH(*decls, enum_decl);

    // Register enum type as compound for typedef emission
    BUF_PUSH(low->compound_types, decl_ast->type);

    lower_methods_into(low, decl_ast->enum_decl.methods, decl_ast->enum_decl.name, decl_ast->type,
                       decls);
}

/** Lower a NODE_FILE into a HIR_FILE with pre-registered fn syms. */
static HirNode *lower_file(Lower *low, const ASTNode *ast) {
    lower_scope_enter(low);
    preregister_fns(low, ast);

    HirNode **decls = NULL;
    for (int32_t i = 0; i < BUF_LEN(ast->file.decls); i++) {
        const ASTNode *decl_ast = ast->file.decls[i];

        if (decl_ast->kind == NODE_STRUCT_DECL) {
            emit_struct_with_methods(low, decl_ast, &decls);
            continue;
        }

        if (decl_ast->kind == NODE_ENUM_DECL) {
            emit_enum_with_methods(low, decl_ast, &decls);
            continue;
        }

        // Pact decls are compile-time only; skip during lower
        if (decl_ast->kind == NODE_PACT_DECL) {
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

HirNode *lower_node(Lower *low, const ASTNode *ast) {
    if (ast == NULL) {
        return NULL;
    }

    switch (ast->kind) {
    case NODE_FILE:
        return lower_file(low, ast);

    case NODE_MODULE: {
        low->current_module = ast->module.name;
        HirNode *node = hir_new(low->hir_arena, HIR_MODULE, &TYPE_UNIT_INST, ast->loc);
        node->module.name = ast->module.name;
        return node;
    }

    case NODE_TYPE_ALIAS: {
        HirNode *node = hir_new(low->hir_arena, HIR_TYPE_ALIAS, &TYPE_UNIT_INST, ast->loc);
        node->type_alias.name = ast->type_alias.name;
        node->type_alias.is_pub = false;
        node->type_alias.underlying = ast->type;
        return node;
    }

    case NODE_FN_DECL:
        return lower_fn_decl(low, ast);

    case NODE_STRUCT_DECL: {
        // Handled primarily in lower_file; fallback for other ctxs
        HirNode *node = hir_new(low->hir_arena, HIR_STRUCT_DECL, &TYPE_UNIT_INST, ast->loc);
        node->struct_decl.name = ast->struct_decl.name;
        node->struct_decl.struct_type = ast->type;
        return node;
    }

    case NODE_ENUM_DECL: {
        // Handled primarily in lower_file; fallback for other ctxs
        HirNode *node = hir_new(low->hir_arena, HIR_ENUM_DECL, &TYPE_UNIT_INST, ast->loc);
        node->enum_decl.name = ast->enum_decl.name;
        node->enum_decl.enum_type = ast->type;
        return node;
    }

    case NODE_PACT_DECL:
        // Pact decls are compile-time only; nothing to lower
        return NULL;

    case NODE_RETURN: {
        HirNode *value = NULL;
        if (ast->return_stmt.value != NULL) {
            value = lower_expr(low, ast->return_stmt.value);
        }
        HirNode *node = hir_new(low->hir_arena, HIR_RETURN,
                                value != NULL ? value->type : &TYPE_UNIT_INST, ast->loc);
        node->return_stmt.value = value;
        return node;
    }

    case NODE_VAR_DECL:
        return lower_var_decl(low, ast);

    case NODE_EXPR_STMT:
        return lower_expr_stmt(low, ast);

    case NODE_BREAK: {
        HirNode *node = hir_new(low->hir_arena, HIR_BREAK, &TYPE_UNIT_INST, ast->loc);
        if (ast->break_stmt.value != NULL) {
            node->break_stmt.value = lower_expr(low, ast->break_stmt.value);
        }
        return node;
    }

    case NODE_CONTINUE:
        return hir_new(low->hir_arena, HIR_CONTINUE, &TYPE_UNIT_INST, ast->loc);

    case NODE_ASSIGN:
        return lower_assign(low, ast);

    case NODE_COMPOUND_ASSIGN:
        return lower_compound_assign(low, ast);

    case NODE_LOOP:
        return lower_loop(low, ast);

    case NODE_WHILE:
        return lower_while(low, ast);

    case NODE_DEFER:
        return lower_defer(low, ast);

    case NODE_FOR:
        return lower_for(low, ast);

    case NODE_BLOCK:
        return lower_block(low, ast);

    case NODE_IF:
        return lower_stmt_if(low, ast);

    default:
        // Expressions at stmt level
        return lower_expr(low, ast);
    }
}
