#include "_lower.h"

// ── HIR node rewriting ────────────────────────────────────────────────

/**
 * Callback that builds a prefix stmt to insert before a target node.
 * Returns NULL if the node should not be rewritten.
 */
typedef HirNode *(*RewritePrefixFn)(Lower *low, HirNode *node, void *ctx);

/**
 * Generic recursive rewriter: walks the HIR tree and, for each node where
 * @p build_prefix returns non-NULL, wraps `{ prefix; node; }` in a block.
 * Stops at nested HIR_LOOP boundaries.
 */
static void rewrite_hir_nodes(Lower *low, HirNode **node_ptr, RewritePrefixFn build_prefix,
                              void *ctx) {
    HirNode *node = *node_ptr;
    if (node == NULL) {
        return;
    }

    HirNode *prefix = build_prefix(low, node, ctx);
    if (prefix != NULL) {
        HirNode **stmts = NULL;
        BUF_PUSH(stmts, prefix);
        BUF_PUSH(stmts, node);

        HirNode *block = hir_new(low->hir_arena, HIR_BLOCK, &TYPE_UNIT_INST, node->loc);
        block->block.stmts = stmts;
        block->block.result = NULL;
        *node_ptr = block;
        return;
    }

    if (node->kind == HIR_LOOP) {
        return;
    }

    switch (node->kind) {
    case HIR_BLOCK:
        for (int32_t i = 0; i < BUF_LEN(node->block.stmts); i++) {
            rewrite_hir_nodes(low, &node->block.stmts[i], build_prefix, ctx);
        }
        if (node->block.result != NULL) {
            rewrite_hir_nodes(low, &node->block.result, build_prefix, ctx);
        }
        break;
    case HIR_IF:
        rewrite_hir_nodes(low, &node->if_expr.then_body, build_prefix, ctx);
        rewrite_hir_nodes(low, &node->if_expr.else_body, build_prefix, ctx);
        break;
    default:
        break;
    }
}

// ── Loop lowering ─────────────────────────────────────────────────────

/** Build an assignment prefix for a break-with-value node. */
static HirNode *break_value_prefix(Lower *low, HirNode *node, void *ctx) {
    HirSym *result_sym = ctx;
    if (node->kind != HIR_BREAK || node->break_stmt.value == NULL) {
        return NULL;
    }
    HirNode *assign = hir_new(low->hir_arena, HIR_ASSIGN, &TYPE_UNIT_INST, node->loc);
    assign->assign.target = lower_make_var_ref(low, result_sym, node->loc);
    assign->assign.value = node->break_stmt.value;
    node->break_stmt.value = NULL;
    return assign;
}

/**
 * Rewrite HIR_BREAK nodes with values: `break val` → `{ result = val; break; }`.
 * Skips nested HIR_LOOP nodes (their breaks are independent).
 */
static void rewrite_break_values(Lower *low, HirNode **node_ptr, HirSym *result_sym) {
    rewrite_hir_nodes(low, node_ptr, break_value_prefix, result_sym);
}

HirNode *lower_loop(Lower *low, const ASTNode *ast) {
    const Type *loop_type = ast->type != NULL ? ast->type : &TYPE_UNIT_INST;
    bool is_expr = loop_type->kind != TYPE_UNIT && loop_type->kind != TYPE_NEVER;

    HirNode *body = lower_block(low, ast->loop.body);

    if (!is_expr) {
        HirNode *node = hir_new(low->hir_arena, HIR_LOOP, loop_type, ast->loc);
        node->loop.body = body;
        return node;
    }

    // Loop expr: desugar break-with-value.
    // { var _result; loop { ... break val → { _result = val; break } ... }; _result }
    const char *result_name = lower_make_temp_name(low);
    HirSymSpec result_spec = {HIR_SYM_VAR, result_name, loop_type, true, ast->loc};
    HirSym *result_sym = lower_add_var(low, &result_spec);
    HirNode *result_decl = lower_make_var_decl(low, result_sym, NULL);

    rewrite_break_values(low, &body, result_sym);

    HirNode *loop_node = hir_new(low->hir_arena, HIR_LOOP, loop_type, ast->loc);
    loop_node->loop.body = body;

    HirNode *result_ref = lower_make_var_ref(low, result_sym, ast->loc);

    HirNode **outer_stmts = NULL;
    BUF_PUSH(outer_stmts, result_decl);
    BUF_PUSH(outer_stmts, loop_node);

    HirNode *outer = hir_new(low->hir_arena, HIR_BLOCK, loop_type, ast->loc);
    outer->block.stmts = outer_stmts;
    outer->block.result = result_ref;
    return outer;
}

// ── While-loop lowering ───────────────────────────────────────────────

/** Desugar `while cond { body }` → `loop { if !cond { break }; body }`. */
HirNode *lower_while(Lower *low, const ASTNode *ast) {
    SrcLoc loc = ast->loc;

    // while-let: desugar to loop { match pattern_init { P => body, _ => break } }
    if (ast->while_loop.pattern != NULL) {
        HirNode *match_operand = lower_expr(low, ast->while_loop.pattern_init);
        const Type *op_type = match_operand->type;

        const char *tmp = lower_make_temp_name(low);
        HirSym *tmp_sym = lower_add_var(low, &(HirSymSpec){HIR_SYM_VAR, tmp, op_type, false, loc});

        HirNode **arm_conds = NULL;
        HirNode **arm_guards = NULL;
        HirNode **arm_bodies = NULL;
        HirNode **arm_bindings = NULL;

        PatternOperand operand = {lower_make_var_ref(low, tmp_sym, loc), tmp_sym, op_type};

        // Arm 0: pattern match → body
        HirNode *cond = lower_pattern_cond(low, ast->while_loop.pattern, &operand, loc);
        BUF_PUSH(arm_conds, cond);
        BUF_PUSH(arm_guards, NULL);

        lower_scope_enter(low);
        lower_pattern_bindings(low, ast->while_loop.pattern, op_type);
        HirNode *body = lower_block(low, ast->while_loop.body);
        HirNode *binds = lower_arm_bindings_block(low, ast->while_loop.pattern, &operand, loc);
        BUF_PUSH(arm_bodies, body);
        BUF_PUSH(arm_bindings, binds);
        lower_scope_leave(low);

        // Arm 1: wildcard → break
        BUF_PUSH(arm_conds, NULL);
        BUF_PUSH(arm_guards, NULL);
        HirNode *break_node = hir_new(low->hir_arena, HIR_BREAK, &TYPE_UNIT_INST, loc);
        HirNode **break_stmts = NULL;
        BUF_PUSH(break_stmts, break_node);
        HirNode *break_block = hir_new(low->hir_arena, HIR_BLOCK, &TYPE_UNIT_INST, loc);
        break_block->block.stmts = break_stmts;
        break_block->block.result = NULL;
        BUF_PUSH(arm_bodies, break_block);
        BUF_PUSH(arm_bindings, NULL);

        HirNode *match = hir_new(low->hir_arena, HIR_MATCH, &TYPE_UNIT_INST, loc);
        match->match_expr.operand = lower_make_var_decl(low, tmp_sym, match_operand);
        match->match_expr.arm_conds = arm_conds;
        match->match_expr.arm_guards = arm_guards;
        match->match_expr.arm_bodies = arm_bodies;
        match->match_expr.arm_bindings = arm_bindings;

        HirNode **loop_stmts = NULL;
        BUF_PUSH(loop_stmts, match);
        HirNode *loop_body = hir_new(low->hir_arena, HIR_BLOCK, &TYPE_UNIT_INST, loc);
        loop_body->block.stmts = loop_stmts;
        loop_body->block.result = NULL;

        HirNode *loop_node = hir_new(low->hir_arena, HIR_LOOP, &TYPE_UNIT_INST, loc);
        loop_node->loop.body = loop_body;
        return loop_node;
    }

    HirNode *cond = lower_expr(low, ast->while_loop.cond);

    // Build: if !cond { break }
    HirNode *neg_cond = hir_new(low->hir_arena, HIR_UNARY, &TYPE_BOOL_INST, loc);
    neg_cond->unary.op = TOKEN_BANG;
    neg_cond->unary.operand = cond;

    HirNode *break_node = hir_new(low->hir_arena, HIR_BREAK, &TYPE_UNIT_INST, loc);
    HirNode **break_stmts = NULL;
    BUF_PUSH(break_stmts, break_node);
    HirNode *break_block = hir_new(low->hir_arena, HIR_BLOCK, &TYPE_UNIT_INST, loc);
    break_block->block.stmts = break_stmts;
    break_block->block.result = NULL;

    HirNode *guard = hir_new(low->hir_arena, HIR_IF, &TYPE_UNIT_INST, loc);
    guard->if_expr.cond = neg_cond;
    guard->if_expr.then_body = break_block;
    guard->if_expr.else_body = NULL;

    // Build loop body: { guard; user_body }
    HirNode *user_body = lower_block(low, ast->while_loop.body);
    HirNode **loop_stmts = NULL;
    BUF_PUSH(loop_stmts, guard);
    for (int32_t i = 0; i < BUF_LEN(user_body->block.stmts); i++) {
        BUF_PUSH(loop_stmts, user_body->block.stmts[i]);
    }
    if (user_body->block.result != NULL) {
        BUF_PUSH(loop_stmts, user_body->block.result);
    }

    HirNode *loop_body = hir_new(low->hir_arena, HIR_BLOCK, &TYPE_UNIT_INST, loc);
    loop_body->block.stmts = loop_stmts;
    loop_body->block.result = NULL;

    HirNode *loop_node = hir_new(low->hir_arena, HIR_LOOP, &TYPE_UNIT_INST, loc);
    loop_node->loop.body = loop_body;
    return loop_node;
}

// ── For-loop helpers ──────────────────────────────────────────────────

/** Build `if iter >= end_bound { break }` guard for desugared for-loop. */
static HirNode *build_for_guard(Lower *low, HirSym *iter_sym, HirSym *end_sym, SrcLoc loc) {
    HirNode *cond = hir_new(low->hir_arena, HIR_BINARY, &TYPE_BOOL_INST, loc);
    cond->binary.op = TOKEN_GREATER_EQUAL;
    cond->binary.left = lower_make_var_ref(low, iter_sym, loc);
    cond->binary.right = lower_make_var_ref(low, end_sym, loc);

    HirNode *break_node = hir_new(low->hir_arena, HIR_BREAK, &TYPE_UNIT_INST, loc);
    HirNode **break_stmts = NULL;
    BUF_PUSH(break_stmts, break_node);
    HirNode *break_block = hir_new(low->hir_arena, HIR_BLOCK, &TYPE_UNIT_INST, loc);
    break_block->block.stmts = break_stmts;
    break_block->block.result = NULL;

    HirNode *guard = hir_new(low->hir_arena, HIR_IF, &TYPE_UNIT_INST, loc);
    guard->if_expr.cond = cond;
    guard->if_expr.then_body = break_block;
    guard->if_expr.else_body = NULL;
    return guard;
}

/** Build `iter = iter + 1` increment for desugared for-loop. */
static HirNode *build_for_increment(Lower *low, HirSym *iter_sym) {
    const Type *iter_type = hir_sym_type(iter_sym);
    SrcLoc loc = iter_sym->loc;
    HirNode *increment = hir_new(low->hir_arena, HIR_BINARY, iter_type, loc);
    increment->binary.op = TOKEN_PLUS;
    increment->binary.left = lower_make_var_ref(low, iter_sym, loc);
    increment->binary.right = lower_make_int_lit(low, &(IntLitSpec){1, iter_type, TYPE_I32, loc});

    HirNode *assign = hir_new(low->hir_arena, HIR_ASSIGN, &TYPE_UNIT_INST, loc);
    assign->assign.target = lower_make_var_ref(low, iter_sym, loc);
    assign->assign.value = increment;
    return assign;
}

/** Build an increment prefix for a continue node inside a for-loop. */
static HirNode *continue_increment_prefix(Lower *low, HirNode *node, void *ctx) {
    HirSym *iter_sym = ctx;
    if (node->kind != HIR_CONTINUE) {
        return NULL;
    }
    return build_for_increment(low, iter_sym);
}

/**
 * Rewrite HIR_CONTINUE nodes inside a for-loop body to include the
 * iterator increment before the continue.  Skips nested HIR_LOOP nodes
 * (their own continue semantics are independent).
 *
 * Transforms: `continue` → `{ i = i + 1; continue; }`
 */
static void rewrite_continue_for_increment(Lower *low, HirNode **node_ptr, HirSym *iter_sym) {
    rewrite_hir_nodes(low, node_ptr, continue_increment_prefix, iter_sym);
}

/**
 * Lower the user's for-loop body, rewrite continue nodes, and append the
 * resulting stmts to @p out_stmts.
 */
static void build_for_user_body(Lower *low, const ASTNode *body_ast, HirSym *var_sym,
                                HirNode ***out_stmts) {
    if (body_ast == NULL || body_ast->kind != NODE_BLOCK) {
        return;
    }
    HirNode *user_body = lower_block(low, body_ast);
    if (user_body == NULL || user_body->kind != HIR_BLOCK) {
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

// ── For-loop lowering ─────────────────────────────────────────────────

/**
 * Desugar `for slice |v| { body }` or `for slice |v, i| { body }`.
 *
 * Result:
 *   { var _s = slice; var _end = _s.len; var _i = 0;
 *     loop { if _i >= _end { break }; var v = _s[_i]; [var idx = _i;] body; _i = _i + 1 } }
 */
static HirNode *lower_for_slice(Lower *low, const ASTNode *ast) {
    SrcLoc loc = ast->loc;
    const Type *iter_type = &TYPE_I32_INST;

    lower_scope_enter(low);

    // var _s = iterable
    HirNode *iterable_expr = lower_expr(low, ast->for_loop.iterable);
    const Type *slice_type = iterable_expr->type;
    const Type *elem_type = (slice_type != NULL && slice_type->kind == TYPE_SLICE)
                                ? slice_type->slice.elem
                                : &TYPE_ERR_INST;

    const char *slice_name = lower_make_temp_name(low);
    HirSymSpec slice_spec = {HIR_SYM_VAR, slice_name, slice_type, false, loc};
    HirSym *slice_sym = lower_add_var(low, &slice_spec);
    HirNode *slice_decl = lower_make_var_decl(low, slice_sym, iterable_expr);

    // var _end = _s.len
    HirNode *len_access =
        lower_make_field_access(low, &(FieldAccessSpec){lower_make_var_ref(low, slice_sym, loc),
                                                        RSG_FIELD_LEN, iter_type, false, loc});

    const char *end_name = lower_make_temp_name(low);
    HirSymSpec end_spec = {HIR_SYM_VAR, end_name, iter_type, false, loc};
    HirSym *end_sym = lower_add_var(low, &end_spec);
    HirNode *end_decl = lower_make_var_decl(low, end_sym, len_access);

    // var _i = 0
    const char *counter_name = lower_make_temp_name(low);
    HirSymSpec counter_spec = {HIR_SYM_VAR, counter_name, iter_type, true, loc};
    HirSym *counter_sym = lower_add_var(low, &counter_spec);
    IntLitSpec zero_spec = {0, iter_type, TYPE_I32, loc};
    HirNode *counter_decl =
        lower_make_var_decl(low, counter_sym, lower_make_int_lit(low, &zero_spec));

    // Build loop body
    HirNode **loop_stmts = NULL;

    // Guard: if _i >= _end { break }
    BUF_PUSH(loop_stmts, build_for_guard(low, counter_sym, end_sym, loc));

    // var v = _s[_i]
    const char *val_name = ast->for_loop.var_name;
    HirNode *idx_ref = lower_make_var_ref(low, counter_sym, loc);
    HirNode *elem_access = hir_new(low->hir_arena, HIR_IDX, elem_type, loc);
    elem_access->idx_access.object = lower_make_var_ref(low, slice_sym, loc);
    elem_access->idx_access.idx = idx_ref;

    HirSymSpec val_spec = {HIR_SYM_VAR, val_name, elem_type, false, loc};
    HirSym *val_sym = lower_add_var(low, &val_spec);
    BUF_PUSH(loop_stmts, lower_make_var_decl(low, val_sym, elem_access));

    // Optional: var idx = _i
    if (ast->for_loop.idx_name != NULL) {
        HirSymSpec idx_spec = {HIR_SYM_VAR, ast->for_loop.idx_name, iter_type, false, loc};
        HirSym *idx_sym = lower_add_var(low, &idx_spec);
        BUF_PUSH(loop_stmts,
                 lower_make_var_decl(low, idx_sym, lower_make_var_ref(low, counter_sym, loc)));
    }

    // User body + continue rewrite + increment
    build_for_user_body(low, ast->for_loop.body, counter_sym, &loop_stmts);
    BUF_PUSH(loop_stmts, build_for_increment(low, counter_sym));

    HirNode *loop_body = hir_new(low->hir_arena, HIR_BLOCK, &TYPE_UNIT_INST, loc);
    loop_body->block.stmts = loop_stmts;
    loop_body->block.result = NULL;

    HirNode *loop_node = hir_new(low->hir_arena, HIR_LOOP, &TYPE_UNIT_INST, loc);
    loop_node->loop.body = loop_body;

    HirNode **outer_stmts = NULL;
    BUF_PUSH(outer_stmts, slice_decl);
    BUF_PUSH(outer_stmts, end_decl);
    BUF_PUSH(outer_stmts, counter_decl);
    BUF_PUSH(outer_stmts, loop_node);

    lower_scope_leave(low);

    HirNode *outer = hir_new(low->hir_arena, HIR_BLOCK, &TYPE_UNIT_INST, loc);
    outer->block.stmts = outer_stmts;
    outer->block.result = NULL;
    return outer;
}

/**
 * Desugar `for var := start..end { body }` into a HIR_LOOP.
 *
 * Result:
 *   { var _end = end; var i = start; loop { if i >= _end { break } body; i = i + 1 } }
 */
static HirNode *lower_for_range(Lower *low, const ASTNode *ast) {
    SrcLoc loc = ast->loc;
    const Type *iter_type = &TYPE_I32_INST;

    lower_scope_enter(low);

    // var _end = end
    HirNode *end_expr = lower_expr(low, ast->for_loop.end);
    const char *end_name = lower_make_temp_name(low);
    HirSymSpec end_spec = {HIR_SYM_VAR, end_name, iter_type, false, loc};
    HirSym *end_sym = lower_add_var(low, &end_spec);
    HirNode *end_decl = lower_make_var_decl(low, end_sym, end_expr);

    // var i = start
    const char *var_name = ast->for_loop.var_name;
    HirNode *start = lower_expr(low, ast->for_loop.start);
    HirSymSpec iter_spec = {HIR_SYM_VAR, var_name, iter_type, true, loc};
    HirSym *var_sym = lower_add_var(low, &iter_spec);
    HirNode *iter_decl = lower_make_var_decl(low, var_sym, start);

    // Build loop body: guard + user body + increment
    HirNode **loop_stmts = NULL;
    BUF_PUSH(loop_stmts, build_for_guard(low, var_sym, end_sym, loc));
    build_for_user_body(low, ast->for_loop.body, var_sym, &loop_stmts);
    BUF_PUSH(loop_stmts, build_for_increment(low, var_sym));

    HirNode *loop_body = hir_new(low->hir_arena, HIR_BLOCK, &TYPE_UNIT_INST, loc);
    loop_body->block.stmts = loop_stmts;
    loop_body->block.result = NULL;

    HirNode *loop_node = hir_new(low->hir_arena, HIR_LOOP, &TYPE_UNIT_INST, loc);
    loop_node->loop.body = loop_body;

    // Outer block: { var _end = ...; var i = ...; loop { ... } }
    HirNode **outer_stmts = NULL;
    BUF_PUSH(outer_stmts, end_decl);
    BUF_PUSH(outer_stmts, iter_decl);
    BUF_PUSH(outer_stmts, loop_node);

    lower_scope_leave(low);

    HirNode *outer = hir_new(low->hir_arena, HIR_BLOCK, &TYPE_UNIT_INST, loc);
    outer->block.stmts = outer_stmts;
    outer->block.result = NULL;
    return outer;
}

/** Dispatch for-loop lowering to slice or range variant. */
HirNode *lower_for(Lower *low, const ASTNode *ast) {
    if (ast->for_loop.iterable != NULL) {
        return lower_for_slice(low, ast);
    }
    return lower_for_range(low, ast);
}
