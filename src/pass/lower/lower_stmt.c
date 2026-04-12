#include "_lower.h"

/**
 * @file lower_stmt.c
 * @brief Statement and control-flow lowering, plus the main lower_node dispatcher.
 *
 * File/module-level lowering lives in lower_file.c.
 */

// ── Statement / control-flow lower ─────────────────────────────────

HirNode *lower_stmt_if(Lower *low, const ASTNode *ast) {
    // if-var: desugar to HIR_MATCH with 2 arms
    if (ast->if_expr.pattern != NULL) {
        HirNode *match_operand = lower_expr(low, ast->if_expr.pattern_init);
        const Type *op_type = match_operand->type;
        SrcLoc loc = ast->loc;

        const char *tmp = lower_make_temp_name(low);
        HirSym *tmp_sym = lower_add_var(low, &(HirSymSpec){HIR_SYM_VAR, tmp, op_type, false, loc});

        HirMatchArm *arms = NULL;

        PatternOperand operand = {lower_make_var_ref(low, tmp_sym, loc), tmp_sym, op_type};

        // Arm 0: pattern match
        HirNode *cond = lower_pattern_cond(low, ast->if_expr.pattern, &operand, loc);

        lower_scope_enter(low);
        lower_pattern_bindings(low, ast->if_expr.pattern, op_type);
        HirNode *then = lower_node(low, ast->if_expr.then_body);
        HirNode *binds = lower_arm_bindings_block(low, ast->if_expr.pattern, &operand, loc);
        BUF_PUSH(arms,
                 ((HirMatchArm){.cond = cond, .guard = NULL, .body = then, .bindings = binds}));
        lower_scope_leave(low);

        // Arm 1: wildcard (else)
        HirNode *else_body = ast->if_expr.else_body != NULL
                                 ? lower_node(low, ast->if_expr.else_body)
                                 : hir_new(low->hir_arena, HIR_UNIT_LIT, &TYPE_UNIT_INST, loc);
        BUF_PUSH(arms,
                 ((HirMatchArm){.cond = NULL, .guard = NULL, .body = else_body, .bindings = NULL}));

        HirNode *match = hir_new(low->hir_arena, HIR_MATCH, ast->type, loc);
        match->match_expr.operand = lower_make_var_decl(low, tmp_sym, match_operand);
        match->match_expr.arms = arms;
        return match;
    }

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
    assert(ast->type != NULL && "lower: var decl type must be set by check");
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
    // Auto-deref: insert deref when target is *T and value matches pointee
    if (target->type != NULL && target->type->kind == TYPE_PTR && value->type != NULL &&
        type_equal(value->type, target->type->ptr.pointee)) {
        HirNode *deref = hir_new(low->hir_arena, HIR_DEREF, target->type->ptr.pointee, ast->loc);
        deref->deref.operand = target;
        target = deref;
    }
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
    TokenKind base_op = token_compound_base_op(ast->compound_assign.op);
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
            field_access = lower_make_field_access(
                low, &(FieldAccessSpec){lower_make_var_ref(low, tmp_sym, ast->loc), fname,
                                        field_type, false, ast->loc});
        } else {
            // Check promoted fields from embedded structs
            HirNode *tmp_ref = lower_make_var_ref(low, tmp_sym, ast->loc);
            FieldLookup lookup = {tmp_ref, struct_type, fname, false, ast->loc};
            field_access = lower_resolve_promoted_field(low, &lookup);
            if (field_access != NULL) {
                field_type = field_access->type;
            }
        }

        assert(field_type != NULL &&
               "lower: struct destructure field type must be resolved after check");
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
        assert(tuple_type != NULL && tuple_type->kind == TYPE_TUPLE && elem_idx < tuple_count &&
               "lower: tuple destructure element type must be resolved after check");
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

// ── Inline lower helpers for simple node kinds ─────────────────────

static HirNode *lower_type_alias(Lower *low, const ASTNode *ast) {
    HirNode *node = hir_new(low->hir_arena, HIR_TYPE_ALIAS, &TYPE_UNIT_INST, ast->loc);
    node->type_alias.name = ast->type_alias.name;
    node->type_alias.is_pub = false;
    node->type_alias.underlying = ast->type;
    return node;
}

static HirNode *lower_struct_decl_node(Lower *low, const ASTNode *ast) {
    if (BUF_LEN(ast->struct_decl->type_params) > 0) {
        return NULL;
    }
    HirNode *node = hir_new(low->hir_arena, HIR_STRUCT_DECL, &TYPE_UNIT_INST, ast->loc);
    node->struct_decl.name = ast->struct_decl->name;
    node->struct_decl.struct_type = ast->type;
    return node;
}

static HirNode *lower_enum_decl_node(Lower *low, const ASTNode *ast) {
    if (BUF_LEN(ast->enum_decl->type_params) > 0) {
        return NULL;
    }
    HirNode *node = hir_new(low->hir_arena, HIR_ENUM_DECL, &TYPE_UNIT_INST, ast->loc);
    node->enum_decl.name = ast->enum_decl->name;
    node->enum_decl.enum_type = ast->type;
    return node;
}

static HirNode *lower_return(Lower *low, const ASTNode *ast) {
    HirNode *value = NULL;
    if (ast->return_stmt.value != NULL) {
        value = lower_expr(low, ast->return_stmt.value);
    }
    HirNode *node = hir_new(low->hir_arena, HIR_RETURN,
                            value != NULL ? value->type : &TYPE_UNIT_INST, ast->loc);
    node->return_stmt.value = value;
    return node;
}

static HirNode *lower_break(Lower *low, const ASTNode *ast) {
    HirNode *node = hir_new(low->hir_arena, HIR_BREAK, &TYPE_UNIT_INST, ast->loc);
    if (ast->break_stmt.value != NULL) {
        node->break_stmt.value = lower_expr(low, ast->break_stmt.value);
    }
    return node;
}

// ── Main dispatcher ────────────────────────────────────────────────

HirNode *lower_node(Lower *low, const ASTNode *ast) {
    if (ast == NULL) {
        return NULL;
    }

    switch (ast->kind) {
    case NODE_FILE:
        return lower_file(low, ast);

    case NODE_MODULE:
        return lower_module(low, ast);

    case NODE_TYPE_ALIAS:
        return lower_type_alias(low, ast);

    case NODE_FN_DECL:
        // Skip generic fn templates — only monomorphized copies are lowered
        if (BUF_LEN(ast->fn_decl.type_params) > 0) {
            return NULL;
        }
        return lower_fn_decl(low, ast);

    case NODE_STRUCT_DECL:
        return lower_struct_decl_node(low, ast);

    case NODE_ENUM_DECL:
        return lower_enum_decl_node(low, ast);

    case NODE_PACT_DECL: // NOLINT(bugprone-branch-clone)
        return NULL;

    case NODE_EXT_DECL:
        return NULL; // handled in lower_file

    case NODE_USE_DECL:
        return NULL; // compile-time only

    case NODE_RETURN:
        return lower_return(low, ast);

    case NODE_VAR_DECL:
        return lower_var_decl(low, ast);

    case NODE_EXPR_STMT:
        return lower_expr_stmt(low, ast);

    case NODE_BREAK:
        return lower_break(low, ast);

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
        return lower_expr(low, ast);
    }
}
