#include "_lowering.h"

// ── Statement / control-flow lowering ─────────────────────────────────

TTNode *lower_stmt_if(Lowering *low, const ASTNode *ast) {
    TTNode *cond = lower_expr(low, ast->if_expr.cond);
    TTNode *then_body = lower_node(low, ast->if_expr.then_body);
    TTNode *else_body = NULL;
    if (ast->if_expr.else_body != NULL) {
        else_body = lower_node(low, ast->if_expr.else_body);
    }
    TTNode *node = tt_new(low->tt_arena, TT_IF, ast->type, ast->loc);
    node->if_expr.cond = cond;
    node->if_expr.then_body = then_body;
    node->if_expr.else_body = else_body;
    return node;
}

static TTNode *lower_var_decl(Lowering *low, const ASTNode *ast) {
    const char *name = ast->var_decl.name;
    const Type *type = ast->type != NULL ? ast->type : &TYPE_ERR_INST;
    bool is_mut = ast->var_decl.is_var;

    TtSymSpec var_spec = {TT_SYM_VAR, name, type, is_mut, ast->loc};
    TTSym *sym = lowering_add_var(low, &var_spec);

    TTNode *init = NULL;
    if (ast->var_decl.init != NULL) {
        init = lower_expr(low, ast->var_decl.init);
    }

    return lowering_make_var_decl(low, sym, init);
}

static TTNode *lower_assign(Lowering *low, const ASTNode *ast) {
    TTNode *target = lower_expr(low, ast->assign.target);
    TTNode *value = lower_expr(low, ast->assign.value);
    TTNode *node = tt_new(low->tt_arena, TT_ASSIGN, &TYPE_UNIT_INST, ast->loc);
    node->assign.target = target;
    node->assign.value = value;
    return node;
}

/** Desugar `x op= expr` → `x = x op expr`. */
static TTNode *lower_compound_assign(Lowering *low, const ASTNode *ast) {
    TTNode *target = lower_expr(low, ast->compound_assign.target);
    TTNode *value = lower_expr(low, ast->compound_assign.value);

    // Build binary: target op value
    TokenKind base_op = lowering_compound_to_base_op(ast->compound_assign.op);
    TTNode *target_read = lower_expr(low, ast->compound_assign.target);
    TTNode *binary = tt_new(low->tt_arena, TT_BINARY, target->type, ast->loc);
    binary->binary.op = base_op;
    binary->binary.left = target_read;
    binary->binary.right = value;

    // Assign: target = binary
    TTNode *node = tt_new(low->tt_arena, TT_ASSIGN, &TYPE_UNIT_INST, ast->loc);
    node->assign.target = target;
    node->assign.value = binary;
    return node;
}

/** Expand a struct destructure into var decls in @p stmts. */
static void lower_struct_destructure_into(Lowering *low, const ASTNode *ast, TTNode ***stmts) {
    TTNode *value = lower_expr(low, ast->struct_destructure.value);
    const Type *struct_type = value->type;

    const char *tmp_name = lowering_make_temp_name(low);
    TtSymSpec tmp_spec = {TT_SYM_VAR, tmp_name, struct_type, false, ast->loc};
    TTSym *tmp_sym = lowering_add_var(low, &tmp_spec);
    BUF_PUSH(*stmts, lowering_make_var_decl(low, tmp_sym, value));

    for (int32_t i = 0; i < BUF_LEN(ast->struct_destructure.field_names); i++) {
        const char *fname = ast->struct_destructure.field_names[i];
        const char *alias = (ast->struct_destructure.aliases != NULL &&
                             i < BUF_LEN(ast->struct_destructure.aliases))
                                ? ast->struct_destructure.aliases[i]
                                : NULL;
        const char *var_name = (alias != NULL) ? alias : fname;
        const Type *field_type = NULL;
        TTNode *field_access = NULL;

        // Check direct fields
        const StructField *sf = type_struct_find_field(struct_type, fname);
        if (sf != NULL) {
            field_type = sf->type;
            field_access = tt_new(low->tt_arena, TT_STRUCT_FIELD_ACCESS, field_type, ast->loc);
            field_access->struct_field_access.object =
                lowering_make_var_ref(low, tmp_sym, ast->loc);
            field_access->struct_field_access.field = fname;
            field_access->struct_field_access.via_ptr = false;
        } else {
            // Check promoted fields from embedded structs
            TTNode *tmp_ref = lowering_make_var_ref(low, tmp_sym, ast->loc);
            FieldLookup lookup = {tmp_ref, struct_type, fname, false, ast->loc};
            field_access = lowering_resolve_promoted_field(low, &lookup);
            if (field_access != NULL) {
                field_type = field_access->type;
            }
        }

        if (field_type == NULL) {
            field_type = &TYPE_ERR_INST;
        }

        TtSymSpec vspec = {TT_SYM_VAR, var_name, field_type, false, ast->loc};
        TTSym *var_sym = lowering_add_var(low, &vspec);
        BUF_PUSH(*stmts, lowering_make_var_decl(low, var_sym, field_access));
    }
}

/** Expand a tuple destructure into var decls in @p stmts. */
static void lower_tuple_destructure_into(Lowering *low, const ASTNode *ast, TTNode ***stmts) {
    TTNode *value = lower_expr(low, ast->tuple_destructure.value);
    const Type *tuple_type = value->type;

    const char *tmp_name = lowering_make_temp_name(low);
    TtSymSpec tmp_spec2 = {TT_SYM_VAR, tmp_name, tuple_type, false, ast->loc};
    TTSym *tmp_sym = lowering_add_var(low, &tmp_spec2);
    BUF_PUSH(*stmts, lowering_make_var_decl(low, tmp_sym, value));

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

        TTNode *idx_access = tt_new(low->tt_arena, TT_TUPLE_IDX, elem_type, ast->loc);
        idx_access->tuple_idx.object = lowering_make_var_ref(low, tmp_sym, ast->loc);
        idx_access->tuple_idx.elem_idx = elem_idx;

        TtSymSpec vspec2 = {TT_SYM_VAR, vname, elem_type, false, ast->loc};
        TTSym *var_sym = lowering_add_var(low, &vspec2);
        BUF_PUSH(*stmts, lowering_make_var_decl(low, var_sym, idx_access));
    }
}

TTNode *lower_block(Lowering *low, const ASTNode *ast) {
    if (ast == NULL) {
        return NULL;
    }
    assert(ast->kind == NODE_BLOCK);
    lowering_scope_enter(low);

    TTNode **stmts = NULL;
    for (int32_t i = 0; i < BUF_LEN(ast->block.stmts); i++) {
        const ASTNode *stmt = ast->block.stmts[i];
        if (stmt->kind == NODE_STRUCT_DESTRUCTURE) {
            lower_struct_destructure_into(low, stmt, &stmts);
        } else if (stmt->kind == NODE_TUPLE_DESTRUCTURE) {
            lower_tuple_destructure_into(low, stmt, &stmts);
        } else {
            TTNode *s = lower_node(low, stmt);
            if (s != NULL) {
                BUF_PUSH(stmts, s);
            }
        }
    }

    TTNode *result = NULL;
    if (ast->block.result != NULL) {
        // Assignments in result pos are side-effect-only stmts.
        if (ast->block.result->kind == NODE_ASSIGN) {
            TTNode *stmt = lower_assign(low, ast->block.result);
            BUF_PUSH(stmts, stmt);
        } else if (ast->block.result->kind == NODE_COMPOUND_ASSIGN) {
            TTNode *stmt = lower_compound_assign(low, ast->block.result);
            BUF_PUSH(stmts, stmt);
        } else {
            result = lower_expr(low, ast->block.result);
        }
    }

    lowering_scope_leave(low);

    TTNode *node =
        tt_new(low->tt_arena, TT_BLOCK, ast->type != NULL ? ast->type : &TYPE_UNIT_INST, ast->loc);
    node->block.stmts = stmts;
    node->block.result = result;
    return node;
}

/**
 * Rewrite TT_BREAK nodes with values: `break val` → `{ result = val; break; }`.
 * Skips nested TT_LOOP nodes (their breaks are independent).
 */
static void rewrite_break_values(Lowering *low, TTNode **node_ptr, TTSym *result_sym) {
    TTNode *node = *node_ptr;
    if (node == NULL) {
        return;
    }

    if (node->kind == TT_BREAK && node->break_stmt.value != NULL) {
        TTNode *assign = tt_new(low->tt_arena, TT_ASSIGN, &TYPE_UNIT_INST, node->loc);
        assign->assign.target = lowering_make_var_ref(low, result_sym, node->loc);
        assign->assign.value = node->break_stmt.value;

        node->break_stmt.value = NULL;

        TTNode **stmts = NULL;
        BUF_PUSH(stmts, assign);
        BUF_PUSH(stmts, node);

        TTNode *block = tt_new(low->tt_arena, TT_BLOCK, &TYPE_UNIT_INST, node->loc);
        block->block.stmts = stmts;
        block->block.result = NULL;
        *node_ptr = block;
        return;
    }

    if (node->kind == TT_LOOP) {
        return;
    }

    switch (node->kind) {
    case TT_BLOCK:
        for (int32_t i = 0; i < BUF_LEN(node->block.stmts); i++) {
            rewrite_break_values(low, &node->block.stmts[i], result_sym);
        }
        if (node->block.result != NULL) {
            rewrite_break_values(low, &node->block.result, result_sym);
        }
        break;
    case TT_IF:
        rewrite_break_values(low, &node->if_expr.then_body, result_sym);
        rewrite_break_values(low, &node->if_expr.else_body, result_sym);
        break;
    default:
        break;
    }
}

/** Desugar `while cond { body }` → `loop { if !cond { break }; body }`. */
static TTNode *lower_while(Lowering *low, const ASTNode *ast) {
    SourceLoc loc = ast->loc;

    TTNode *cond = lower_expr(low, ast->while_loop.cond);

    // Build: if !cond { break }
    TTNode *neg_cond = tt_new(low->tt_arena, TT_UNARY, &TYPE_BOOL_INST, loc);
    neg_cond->unary.op = TOKEN_BANG;
    neg_cond->unary.operand = cond;

    TTNode *break_node = tt_new(low->tt_arena, TT_BREAK, &TYPE_UNIT_INST, loc);
    TTNode **break_stmts = NULL;
    BUF_PUSH(break_stmts, break_node);
    TTNode *break_block = tt_new(low->tt_arena, TT_BLOCK, &TYPE_UNIT_INST, loc);
    break_block->block.stmts = break_stmts;
    break_block->block.result = NULL;

    TTNode *guard = tt_new(low->tt_arena, TT_IF, &TYPE_UNIT_INST, loc);
    guard->if_expr.cond = neg_cond;
    guard->if_expr.then_body = break_block;
    guard->if_expr.else_body = NULL;

    // Build loop body: { guard; user_body }
    TTNode *user_body = lower_block(low, ast->while_loop.body);
    TTNode **loop_stmts = NULL;
    BUF_PUSH(loop_stmts, guard);
    for (int32_t i = 0; i < BUF_LEN(user_body->block.stmts); i++) {
        BUF_PUSH(loop_stmts, user_body->block.stmts[i]);
    }
    if (user_body->block.result != NULL) {
        BUF_PUSH(loop_stmts, user_body->block.result);
    }

    TTNode *loop_body = tt_new(low->tt_arena, TT_BLOCK, &TYPE_UNIT_INST, loc);
    loop_body->block.stmts = loop_stmts;
    loop_body->block.result = NULL;

    TTNode *loop_node = tt_new(low->tt_arena, TT_LOOP, &TYPE_UNIT_INST, loc);
    loop_node->loop.body = loop_body;
    return loop_node;
}

static TTNode *lower_defer(Lowering *low, const ASTNode *ast) {
    TTNode *body = lower_node(low, ast->defer_stmt.body);
    TTNode *node = tt_new(low->tt_arena, TT_DEFER, &TYPE_UNIT_INST, ast->loc);
    node->defer_stmt.body = body;
    return node;
}

static TTNode *lower_loop(Lowering *low, const ASTNode *ast) {
    const Type *loop_type = ast->type != NULL ? ast->type : &TYPE_UNIT_INST;
    bool is_expr = loop_type->kind != TYPE_UNIT && loop_type->kind != TYPE_NEVER;

    TTNode *body = lower_block(low, ast->loop.body);

    if (!is_expr) {
        TTNode *node = tt_new(low->tt_arena, TT_LOOP, loop_type, ast->loc);
        node->loop.body = body;
        return node;
    }

    // Loop expr: desugar break-with-value.
    // { var _result; loop { ... break val → { _result = val; break } ... }; _result }
    const char *result_name = lowering_make_temp_name(low);
    TtSymSpec result_spec = {TT_SYM_VAR, result_name, loop_type, true, ast->loc};
    TTSym *result_sym = lowering_add_var(low, &result_spec);
    TTNode *result_decl = lowering_make_var_decl(low, result_sym, NULL);

    rewrite_break_values(low, &body, result_sym);

    TTNode *loop_node = tt_new(low->tt_arena, TT_LOOP, loop_type, ast->loc);
    loop_node->loop.body = body;

    TTNode *result_ref = lowering_make_var_ref(low, result_sym, ast->loc);

    TTNode **outer_stmts = NULL;
    BUF_PUSH(outer_stmts, result_decl);
    BUF_PUSH(outer_stmts, loop_node);

    TTNode *outer = tt_new(low->tt_arena, TT_BLOCK, loop_type, ast->loc);
    outer->block.stmts = outer_stmts;
    outer->block.result = result_ref;
    return outer;
}

/** Build `if iter >= end_bound { break }` guard for desugared for-loop. */
static TTNode *build_for_guard(Lowering *low, TTSym *iter_sym, TTSym *end_sym, SourceLoc loc) {
    TTNode *cond = tt_new(low->tt_arena, TT_BINARY, &TYPE_BOOL_INST, loc);
    cond->binary.op = TOKEN_GREATER_EQUAL;
    cond->binary.left = lowering_make_var_ref(low, iter_sym, loc);
    cond->binary.right = lowering_make_var_ref(low, end_sym, loc);

    TTNode *break_node = tt_new(low->tt_arena, TT_BREAK, &TYPE_UNIT_INST, loc);
    TTNode **break_stmts = NULL;
    BUF_PUSH(break_stmts, break_node);
    TTNode *break_block = tt_new(low->tt_arena, TT_BLOCK, &TYPE_UNIT_INST, loc);
    break_block->block.stmts = break_stmts;
    break_block->block.result = NULL;

    TTNode *guard = tt_new(low->tt_arena, TT_IF, &TYPE_UNIT_INST, loc);
    guard->if_expr.cond = cond;
    guard->if_expr.then_body = break_block;
    guard->if_expr.else_body = NULL;
    return guard;
}

/** Build `iter = iter + 1` increment for desugared for-loop. */
static TTNode *build_for_increment(Lowering *low, TTSym *iter_sym) {
    const Type *iter_type = tt_sym_type(iter_sym);
    SourceLoc loc = iter_sym->loc;
    TTNode *increment = tt_new(low->tt_arena, TT_BINARY, iter_type, loc);
    increment->binary.op = TOKEN_PLUS;
    increment->binary.left = lowering_make_var_ref(low, iter_sym, loc);
    increment->binary.right =
        lowering_make_int_lit(low, &(IntLitSpec){1, iter_type, TYPE_I32, loc});

    TTNode *assign = tt_new(low->tt_arena, TT_ASSIGN, &TYPE_UNIT_INST, loc);
    assign->assign.target = lowering_make_var_ref(low, iter_sym, loc);
    assign->assign.value = increment;
    return assign;
}

/**
 * Rewrite TT_CONTINUE nodes inside a for-loop body to include the
 * iterator increment before the continue.  Skips nested TT_LOOP nodes
 * (their own continue semantics are independent).
 *
 * Transforms: `continue` → `{ i = i + 1; continue; }`
 */
static void rewrite_continue_for_increment(Lowering *low, TTNode **node_ptr, TTSym *iter_sym) {
    TTNode *node = *node_ptr;
    if (node == NULL) {
        return;
    }

    if (node->kind == TT_CONTINUE) {
        // Build: { i = i + 1; continue; }
        TTNode *assign = build_for_increment(low, iter_sym);

        TTNode **stmts = NULL;
        BUF_PUSH(stmts, assign);
        BUF_PUSH(stmts, node); // the original TT_CONTINUE

        TTNode *block = tt_new(low->tt_arena, TT_BLOCK, &TYPE_UNIT_INST, node->loc);
        block->block.stmts = stmts;
        block->block.result = NULL;
        *node_ptr = block;
        return;
    }

    // Do not descend into nested loops — their continue is independent
    if (node->kind == TT_LOOP) {
        return;
    }

    // Recurse into children
    switch (node->kind) {
    case TT_BLOCK:
        for (int32_t i = 0; i < BUF_LEN(node->block.stmts); i++) {
            rewrite_continue_for_increment(low, &node->block.stmts[i], iter_sym);
        }
        if (node->block.result != NULL) {
            rewrite_continue_for_increment(low, &node->block.result, iter_sym);
        }
        break;
    case TT_IF:
        rewrite_continue_for_increment(low, &node->if_expr.then_body, iter_sym);
        rewrite_continue_for_increment(low, &node->if_expr.else_body, iter_sym);
        break;
    default:
        break;
    }
}

/**
 * Lower the user's for-loop body, rewrite continue nodes, and append the
 * resulting stmts to @p out_stmts.
 */
static void build_for_user_body(Lowering *low, const ASTNode *body_ast, TTSym *var_sym,
                                TTNode ***out_stmts) {
    if (body_ast == NULL || body_ast->kind != NODE_BLOCK) {
        return;
    }
    TTNode *user_body = lower_block(low, body_ast);
    if (user_body == NULL || user_body->kind != TT_BLOCK) {
        return;
    }
    rewrite_continue_for_increment(low, &user_body, var_sym);
    for (int32_t i = 0; i < BUF_LEN(user_body->block.stmts); i++) {
        BUF_PUSH(*out_stmts, user_body->block.stmts[i]);
    }
    if (user_body->block.result != NULL) {
        BUF_PUSH(*out_stmts, user_body->block.result);
    }
}

/**
 * Lower `for var := start..end { body }` into desugared TT_LOOP.
 *
 * Desugaring (range):
 *   { var _end = end; var i = start; loop { if i >= _end { break } body; i = i + 1 } }
 *
 * Desugaring (slice):
 *   { var _s = slice; var _end = _s.len; var _i = 0;
 *     loop { if _i >= _end { break }; var v = _s[_i]; [var idx = _i;] body; _i = _i + 1 } }
 */
static TTNode *lower_for(Lowering *low, const ASTNode *ast) {
    SourceLoc loc = ast->loc;
    const Type *iter_type = &TYPE_I32_INST;

    // Slice iteration
    if (ast->for_loop.iterable != NULL) {
        lowering_scope_enter(low);

        // var _s = iterable
        TTNode *iterable_expr = lower_expr(low, ast->for_loop.iterable);
        const Type *slice_type = iterable_expr->type;
        const Type *elem_type = (slice_type != NULL && slice_type->kind == TYPE_SLICE)
                                    ? slice_type->slice.elem
                                    : &TYPE_ERR_INST;

        const char *slice_name = lowering_make_temp_name(low);
        TtSymSpec slice_spec = {TT_SYM_VAR, slice_name, slice_type, false, loc};
        TTSym *slice_sym = lowering_add_var(low, &slice_spec);
        TTNode *slice_decl = lowering_make_var_decl(low, slice_sym, iterable_expr);

        // var _end = _s.len
        TTNode *len_access = tt_new(low->tt_arena, TT_STRUCT_FIELD_ACCESS, iter_type, loc);
        len_access->struct_field_access.object = lowering_make_var_ref(low, slice_sym, loc);
        len_access->struct_field_access.field = "len";
        len_access->struct_field_access.via_ptr = false;

        const char *end_name = lowering_make_temp_name(low);
        TtSymSpec end_spec = {TT_SYM_VAR, end_name, iter_type, false, loc};
        TTSym *end_sym = lowering_add_var(low, &end_spec);
        TTNode *end_decl = lowering_make_var_decl(low, end_sym, len_access);

        // var _i = 0
        const char *counter_name = lowering_make_temp_name(low);
        TtSymSpec counter_spec = {TT_SYM_VAR, counter_name, iter_type, true, loc};
        TTSym *counter_sym = lowering_add_var(low, &counter_spec);
        IntLitSpec zero_spec = {0, iter_type, TYPE_I32, loc};
        TTNode *counter_decl =
            lowering_make_var_decl(low, counter_sym, lowering_make_int_lit(low, &zero_spec));

        // Build loop body
        TTNode **loop_stmts = NULL;

        // Guard: if _i >= _end { break }
        BUF_PUSH(loop_stmts, build_for_guard(low, counter_sym, end_sym, loc));

        // var v = _s[_i]
        const char *val_name = ast->for_loop.var_name;
        TTNode *idx_ref = lowering_make_var_ref(low, counter_sym, loc);
        TTNode *elem_access = tt_new(low->tt_arena, TT_IDX, elem_type, loc);
        elem_access->idx_access.object = lowering_make_var_ref(low, slice_sym, loc);
        elem_access->idx_access.idx = idx_ref;

        TtSymSpec val_spec = {TT_SYM_VAR, val_name, elem_type, false, loc};
        TTSym *val_sym = lowering_add_var(low, &val_spec);
        BUF_PUSH(loop_stmts, lowering_make_var_decl(low, val_sym, elem_access));

        // Optional: var idx = _i
        if (ast->for_loop.idx_name != NULL) {
            TtSymSpec idx_spec = {TT_SYM_VAR, ast->for_loop.idx_name, iter_type, false, loc};
            TTSym *idx_sym = lowering_add_var(low, &idx_spec);
            BUF_PUSH(loop_stmts, lowering_make_var_decl(
                                     low, idx_sym, lowering_make_var_ref(low, counter_sym, loc)));
        }

        // User body + continue rewrite + increment
        build_for_user_body(low, ast->for_loop.body, counter_sym, &loop_stmts);
        BUF_PUSH(loop_stmts, build_for_increment(low, counter_sym));

        TTNode *loop_body = tt_new(low->tt_arena, TT_BLOCK, &TYPE_UNIT_INST, loc);
        loop_body->block.stmts = loop_stmts;
        loop_body->block.result = NULL;

        TTNode *loop_node = tt_new(low->tt_arena, TT_LOOP, &TYPE_UNIT_INST, loc);
        loop_node->loop.body = loop_body;

        TTNode **outer_stmts = NULL;
        BUF_PUSH(outer_stmts, slice_decl);
        BUF_PUSH(outer_stmts, end_decl);
        BUF_PUSH(outer_stmts, counter_decl);
        BUF_PUSH(outer_stmts, loop_node);

        lowering_scope_leave(low);

        TTNode *outer = tt_new(low->tt_arena, TT_BLOCK, &TYPE_UNIT_INST, loc);
        outer->block.stmts = outer_stmts;
        outer->block.result = NULL;
        return outer;
    }

    // Range iteration
    lowering_scope_enter(low);

    // var _end = end
    TTNode *end_expr = lower_expr(low, ast->for_loop.end);
    const char *end_name = lowering_make_temp_name(low);
    TtSymSpec end_spec = {TT_SYM_VAR, end_name, iter_type, false, loc};
    TTSym *end_sym = lowering_add_var(low, &end_spec);
    TTNode *end_decl = lowering_make_var_decl(low, end_sym, end_expr);

    // var i = start
    const char *var_name = ast->for_loop.var_name;
    TTNode *start = lower_expr(low, ast->for_loop.start);
    TtSymSpec iter_spec = {TT_SYM_VAR, var_name, iter_type, true, loc};
    TTSym *var_sym = lowering_add_var(low, &iter_spec);
    TTNode *iter_decl = lowering_make_var_decl(low, var_sym, start);

    // Build loop body: guard + user body + increment
    TTNode **loop_stmts = NULL;
    BUF_PUSH(loop_stmts, build_for_guard(low, var_sym, end_sym, loc));
    build_for_user_body(low, ast->for_loop.body, var_sym, &loop_stmts);
    BUF_PUSH(loop_stmts, build_for_increment(low, var_sym));

    TTNode *loop_body = tt_new(low->tt_arena, TT_BLOCK, &TYPE_UNIT_INST, loc);
    loop_body->block.stmts = loop_stmts;
    loop_body->block.result = NULL;

    TTNode *loop_node = tt_new(low->tt_arena, TT_LOOP, &TYPE_UNIT_INST, loc);
    loop_node->loop.body = loop_body;

    // Outer block: { var _end = ...; var i = ...; loop { ... } }
    TTNode **outer_stmts = NULL;
    BUF_PUSH(outer_stmts, end_decl);
    BUF_PUSH(outer_stmts, iter_decl);
    BUF_PUSH(outer_stmts, loop_node);

    lowering_scope_leave(low);

    TTNode *outer = tt_new(low->tt_arena, TT_BLOCK, &TYPE_UNIT_INST, loc);
    outer->block.stmts = outer_stmts;
    outer->block.result = NULL;
    return outer;
}

static TTNode *lower_expr_stmt(Lowering *low, const ASTNode *ast) {
    const ASTNode *inner = ast->expr_stmt.expr;

    // Assignments are stmts in TT — unwrap and lower directly.
    if (inner->kind == NODE_ASSIGN) {
        return lower_assign(low, inner);
    }
    if (inner->kind == NODE_COMPOUND_ASSIGN) {
        return lower_compound_assign(low, inner);
    }

    return lower_expr(low, inner);
}

/** Pre-register method syms for a named type (struct or enum). */
static void preregister_type_methods(Lowering *low, const char *type_name,
                                     ASTNode *const *methods) {
    for (int32_t j = 0; j < BUF_LEN(methods); j++) {
        const ASTNode *method = methods[j];
        const char *method_name = method->fn_decl.name;
        const Type *ret = method->type != NULL ? method->type : &TYPE_UNIT_INST;
        const char *key = arena_sprintf(low->tt_arena, "%s.%s", type_name, method_name);
        const char *mangled = arena_sprintf(low->tt_arena, "rsgu_%s_%s", type_name, method_name);
        TtSymSpec sym_spec = {TT_SYM_FN, key, ret, false, method->loc};
        TTSym *sym = lowering_make_sym(low, &sym_spec);
        sym->mangled_name = mangled;
        lowering_scope_add(low, key, sym);
    }
}

/** Pre-register all fn decls into scope before lowering bodies. */
static void preregister_fns(Lowering *low, const ASTNode *file_ast) {
    for (int32_t i = 0; i < BUF_LEN(file_ast->file.decls); i++) {
        const ASTNode *decl = file_ast->file.decls[i];
        if (decl->kind == NODE_FN_DECL) {
            const Type *ret = decl->type != NULL ? decl->type : &TYPE_UNIT_INST;
            TtSymSpec fn_spec = {TT_SYM_FN, decl->fn_decl.name, ret, false, decl->loc};
            TTSym *sym = lowering_make_sym(low, &fn_spec);
            sym->mangled_name = arena_sprintf(low->tt_arena, "rsgu_%s", decl->fn_decl.name);
            lowering_scope_add(low, decl->fn_decl.name, sym);
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
static void lower_methods_into(Lowering *low, ASTNode *const *methods, const char *type_name,
                               const Type *type, TTNode ***decls) {
    for (int32_t j = 0; j < BUF_LEN(methods); j++) {
        TTNode *method = lower_method_decl(low, methods[j], type_name, type);
        if (method != NULL) {
            BUF_PUSH(*decls, method);
        }
    }
}

/** Emit a struct type decl and its methods into @p decls. */
static void emit_struct_with_methods(Lowering *low, const ASTNode *decl_ast, TTNode ***decls) {
    TTNode *struct_decl = tt_new(low->tt_arena, TT_STRUCT_DECL, &TYPE_UNIT_INST, decl_ast->loc);
    struct_decl->struct_decl.name = decl_ast->struct_decl.name;
    struct_decl->struct_decl.struct_type = decl_ast->type;
    BUF_PUSH(*decls, struct_decl);

    lower_methods_into(low, decl_ast->struct_decl.methods, decl_ast->struct_decl.name,
                       decl_ast->type, decls);
}

/** Emit an enum type decl and its methods into @p decls. */
static void emit_enum_with_methods(Lowering *low, const ASTNode *decl_ast, TTNode ***decls) {
    TTNode *enum_decl = tt_new(low->tt_arena, TT_ENUM_DECL, &TYPE_UNIT_INST, decl_ast->loc);
    enum_decl->enum_decl.name = decl_ast->enum_decl.name;
    enum_decl->enum_decl.enum_type = decl_ast->type;
    BUF_PUSH(*decls, enum_decl);

    // Register enum type as compound for typedef emission
    BUF_PUSH(low->compound_types, decl_ast->type);

    lower_methods_into(low, decl_ast->enum_decl.methods, decl_ast->enum_decl.name, decl_ast->type,
                       decls);
}

/** Lower a NODE_FILE into a TT_FILE with pre-registered fn syms. */
static TTNode *lower_file(Lowering *low, const ASTNode *ast) {
    lowering_scope_enter(low);
    preregister_fns(low, ast);

    TTNode **decls = NULL;
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

        // Pact decls are compile-time only; skip during lowering
        if (decl_ast->kind == NODE_PACT_DECL) {
            continue;
        }

        TTNode *decl = lower_node(low, decl_ast);
        if (decl != NULL) {
            BUF_PUSH(decls, decl);
        }
    }
    lowering_scope_leave(low);

    TTNode *file_node = tt_new(low->tt_arena, TT_FILE, &TYPE_UNIT_INST, ast->loc);
    file_node->file.decls = decls;
    return file_node;
}

TTNode *lower_node(Lowering *low, const ASTNode *ast) {
    if (ast == NULL) {
        return NULL;
    }

    switch (ast->kind) {
    case NODE_FILE:
        return lower_file(low, ast);

    case NODE_MODULE: {
        low->current_module = ast->module.name;
        TTNode *node = tt_new(low->tt_arena, TT_MODULE, &TYPE_UNIT_INST, ast->loc);
        node->module.name = ast->module.name;
        return node;
    }

    case NODE_TYPE_ALIAS: {
        TTNode *node = tt_new(low->tt_arena, TT_TYPE_ALIAS, &TYPE_UNIT_INST, ast->loc);
        node->type_alias.name = ast->type_alias.name;
        node->type_alias.is_pub = false;
        node->type_alias.underlying = ast->type;
        return node;
    }

    case NODE_FN_DECL:
        return lower_fn_decl(low, ast);

    case NODE_STRUCT_DECL: {
        // Handled primarily in lower_file; fallback for other ctxs
        TTNode *node = tt_new(low->tt_arena, TT_STRUCT_DECL, &TYPE_UNIT_INST, ast->loc);
        node->struct_decl.name = ast->struct_decl.name;
        node->struct_decl.struct_type = ast->type;
        return node;
    }

    case NODE_ENUM_DECL: {
        // Handled primarily in lower_file; fallback for other ctxs
        TTNode *node = tt_new(low->tt_arena, TT_ENUM_DECL, &TYPE_UNIT_INST, ast->loc);
        node->enum_decl.name = ast->enum_decl.name;
        node->enum_decl.enum_type = ast->type;
        return node;
    }

    case NODE_PACT_DECL:
        // Pact decls are compile-time only; nothing to lower
        return NULL;

    case NODE_RETURN: {
        TTNode *value = NULL;
        if (ast->return_stmt.value != NULL) {
            value = lower_expr(low, ast->return_stmt.value);
        }
        TTNode *node = tt_new(low->tt_arena, TT_RETURN,
                              value != NULL ? value->type : &TYPE_UNIT_INST, ast->loc);
        node->return_stmt.value = value;
        return node;
    }

    case NODE_VAR_DECL:
        return lower_var_decl(low, ast);

    case NODE_EXPR_STMT:
        return lower_expr_stmt(low, ast);

    case NODE_BREAK: {
        TTNode *node = tt_new(low->tt_arena, TT_BREAK, &TYPE_UNIT_INST, ast->loc);
        if (ast->break_stmt.value != NULL) {
            node->break_stmt.value = lower_expr(low, ast->break_stmt.value);
        }
        return node;
    }

    case NODE_CONTINUE:
        return tt_new(low->tt_arena, TT_CONTINUE, &TYPE_UNIT_INST, ast->loc);

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
