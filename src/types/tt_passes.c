#include "types/tt_passes.h"

#include <string.h>

// ── Constant folding ──────────────────────────────────────────────────

/** Try folding a binary op on two integer lits. Returns a new node or NULL. */
static TTNode *fold_int_binary(Arena *arena, const TTNode *node) {
    const TTNode *left = node->binary.left;
    const TTNode *right = node->binary.right;

    if (left->kind != TT_INT_LIT || right->kind != TT_INT_LIT) {
        return NULL;
    }
    if (left->int_lit.int_kind != right->int_lit.int_kind) {
        return NULL;
    }

    TypeKind kind = left->int_lit.int_kind;
    int64_t lv = (int64_t)left->int_lit.value;
    int64_t rv = (int64_t)right->int_lit.value;
    int64_t result;
    bool folded = true;

    switch (node->binary.op) {
    case TOKEN_PLUS:
        result = lv + rv;
        break;
    case TOKEN_MINUS:
        result = lv - rv;
        break;
    case TOKEN_STAR:
        result = lv * rv;
        break;
    case TOKEN_SLASH:
        folded = (rv != 0);
        if (folded) {
            result = lv / rv;
        }
        break;
    case TOKEN_PERCENT:
        folded = (rv != 0);
        if (folded) {
            result = lv % rv;
        }
        break;
    default:
        folded = false;
        break;
    }
    if (!folded) {
        return NULL;
    }

    TTNode *lit = tt_new(arena, TT_INT_LIT, node->type, node->loc);
    lit->int_lit.value = (uint64_t)result;
    lit->int_lit.int_kind = kind;
    return lit;
}

/** Try folding a binary op on two float lits. Returns a new node or NULL. */
static TTNode *fold_float_binary(Arena *arena, const TTNode *node) {
    const TTNode *left = node->binary.left;
    const TTNode *right = node->binary.right;

    if (left->kind != TT_FLOAT_LIT || right->kind != TT_FLOAT_LIT) {
        return NULL;
    }
    if (left->float_lit.float_kind != right->float_lit.float_kind) {
        return NULL;
    }

    TypeKind kind = left->float_lit.float_kind;
    double lv = left->float_lit.value;
    double rv = right->float_lit.value;
    double result;
    bool folded = true;

    switch (node->binary.op) {
    case TOKEN_PLUS:
        result = lv + rv;
        break;
    case TOKEN_MINUS:
        result = lv - rv;
        break;
    case TOKEN_STAR:
        result = lv * rv;
        break;
    case TOKEN_SLASH:
        result = lv / rv; // IEEE 754 handles div-by-zero
        break;
    default:
        folded = false;
        break;
    }
    if (!folded) {
        return NULL;
    }

    TTNode *lit = tt_new(arena, TT_FLOAT_LIT, node->type, node->loc);
    lit->float_lit.value = result;
    lit->float_lit.float_kind = kind;
    return lit;
}

/** Try folding a unary negation on an integer or float lit. */
static TTNode *fold_unary_negate(Arena *arena, const TTNode *node) {
    if (node->unary.op != TOKEN_MINUS) {
        return NULL;
    }
    const TTNode *operand = node->unary.operand;
    if (operand->kind == TT_INT_LIT) {
        TTNode *lit = tt_new(arena, TT_INT_LIT, node->type, node->loc);
        lit->int_lit.value = (uint64_t)(-(int64_t)operand->int_lit.value);
        lit->int_lit.int_kind = operand->int_lit.int_kind;
        return lit;
    }
    if (operand->kind == TT_FLOAT_LIT) {
        TTNode *lit = tt_new(arena, TT_FLOAT_LIT, node->type, node->loc);
        lit->float_lit.value = -operand->float_lit.value;
        lit->float_lit.float_kind = operand->float_lit.float_kind;
        return lit;
    }
    return NULL;
}

static void const_fold(Arena *arena, TTNode **node_ptr);

/** Visitor adapter: fold constants in each child node. */
static void const_fold_visitor(void *ctx, TTNode **child_ptr) {
    const_fold((Arena *)ctx, child_ptr);
}

/** Recursively fold constants in @p node_ptr, replacing in-place. */
static void const_fold(Arena *arena, TTNode **node_ptr) {
    TTNode *node = *node_ptr;
    if (node == NULL) {
        return;
    }

    // Walk children first (bottom-up folding)
    tt_visit_children(node, const_fold_visitor, arena);

    // Attempt folding at this node
    TTNode *folded = NULL;
    if (node->kind == TT_BINARY) {
        folded = fold_int_binary(arena, node);
        if (folded == NULL) {
            folded = fold_float_binary(arena, node);
        }
    } else if (node->kind == TT_UNARY) {
        folded = fold_unary_negate(arena, node);
    }
    if (folded != NULL) {
        *node_ptr = folded;
    }
}

void tt_pass_const_fold(Arena *arena, TTNode *root) {
    const_fold(arena, &root);
}

// ── Escape analysis ───────────────────────────────────────────────────

/**
 * Per-fn escape state: tracks which TTSym ptrs hold the
 * result of a TT_ADDRESS_OF, and which TT_ADDRESS_OF nodes escape.
 */

/** Maximum number of address-of nodes tracked per fn. */
#define EA_MAX_ADDR 256

/** Maximum number of tainted syms (vars holding address-of results). */
#define EA_MAX_TAINT 256

typedef struct {
    Arena *arena;

    // All TT_ADDRESS_OF node ptrs found in the current fn body.
    TTNode **addr_nodes[EA_MAX_ADDR];
    int32_t addr_count;

    // Syms tainted with an address-of result (TTSym ptrs).
    const TTSym *tainted[EA_MAX_TAINT];
    // For each tainted sym, the idx into addr_nodes of the feeding
    // TT_ADDRESS_OF (so we can promote the right one).
    int32_t tainted_addr_idx[EA_MAX_TAINT];
    int32_t taint_count;

    // Bitmap: which addr_nodes entries must be promoted.
    bool promote[EA_MAX_ADDR];
} EscapeCtx;

/** Return true if @p sym is in the tainted set; write its addr idx to @p out_idx. */
static bool ea_is_tainted(const EscapeCtx *ctx, const TTSym *sym, int32_t *out_idx) {
    for (int32_t i = 0; i < ctx->taint_count; i++) {
        if (ctx->tainted[i] == sym) {
            if (out_idx != NULL) {
                *out_idx = ctx->tainted_addr_idx[i];
            }
            return true;
        }
    }
    return false;
}

/** Record a tainted sym ← address-of mapping. */
static void ea_add_taint(EscapeCtx *ctx, const TTSym *sym, int32_t addr_idx) {
    if (ctx->taint_count < EA_MAX_TAINT) {
        ctx->tainted[ctx->taint_count] = sym;
        ctx->tainted_addr_idx[ctx->taint_count] = addr_idx;
        ctx->taint_count++;
    }
}

/** Record a TT_ADDRESS_OF node ptr for later promotion. */
static int32_t ea_add_addr(EscapeCtx *ctx, TTNode **node_ptr) {
    if (ctx->addr_count < EA_MAX_ADDR) {
        int32_t idx = ctx->addr_count;
        ctx->addr_nodes[idx] = node_ptr;
        ctx->promote[idx] = false;
        ctx->addr_count++;
        return idx;
    }
    return -1;
}

/**
 * Mark the expr at @p node as escaping.  If it is a direct
 * TT_ADDRESS_OF or a var ref to a tainted sym,
 * flag the corresponding address-of node for heap promotion.
 */
static void ea_mark_escaping(EscapeCtx *ctx, const TTNode *node) {
    if (node == NULL) {
        return;
    }
    if (node->kind == TT_ADDRESS_OF) {
        // Direct address-of in escaping pos — find it in addr list.
        for (int32_t i = 0; i < ctx->addr_count; i++) {
            if (*ctx->addr_nodes[i] == node) {
                ctx->promote[i] = true;
                return;
            }
        }
        return;
    }
    if (node->kind == TT_VAR_REF) {
        int32_t idx;
        if (ea_is_tainted(ctx, node->var_ref.sym, &idx)) {
            ctx->promote[idx] = true;
        }
        return;
    }
    // For if-expr results, both branches may escape.
    if (node->kind == TT_IF) {
        ea_mark_escaping(ctx, node->if_expr.then_body);
        ea_mark_escaping(ctx, node->if_expr.else_body);
        return;
    }
    // For block results, the result expr escapes.
    if (node->kind == TT_BLOCK && node->block.result != NULL) {
        ea_mark_escaping(ctx, node->block.result);
        return;
    }
    // For match exprs, each arm body may escape.
    if (node->kind == TT_MATCH) {
        for (int32_t i = 0; i < BUF_LEN(node->match_expr.arm_bodies); i++) {
            ea_mark_escaping(ctx, node->match_expr.arm_bodies[i]);
        }
        return;
    }
}

// ── Phase 1: collect address-of nodes and build taint map ──

static void ea_collect(EscapeCtx *ctx, TTNode **node_ptr);

static void ea_collect_visitor(void *raw_ctx, TTNode **child_ptr) {
    ea_collect((EscapeCtx *)raw_ctx, child_ptr);
}

static void ea_collect(EscapeCtx *ctx, TTNode **node_ptr) {
    TTNode *node = *node_ptr;
    if (node == NULL) {
        return;
    }

    // Walk children first (bottom-up) so inner ADDRESS_OF nodes are
    // recorded before the enclosing decl tries to ref them.
    // Skip nested fn decls — they have their own scopes.
    if (node->kind != TT_FN_DECL) {
        tt_visit_children(node, ea_collect_visitor, ctx);
    }

    // Record every TT_ADDRESS_OF.
    if (node->kind == TT_ADDRESS_OF) {
        ea_add_addr(ctx, node_ptr);
    }

    // If a var is initialised with a TT_ADDRESS_OF, taint the sym.
    if (node->kind == TT_VAR_DECL && node->var_decl.init != NULL) {
        TTNode *init = node->var_decl.init;
        if (init->kind == TT_ADDRESS_OF) {
            int32_t addr_idx = ctx->addr_count - 1;
            if (addr_idx >= 0 && *ctx->addr_nodes[addr_idx] == init) {
                ea_add_taint(ctx, node->var_decl.sym, addr_idx);
            }
        }
    }

    // If a var is assigned a TT_ADDRESS_OF or tainted value, taint it.
    if (node->kind == TT_ASSIGN) {
        TTNode *val = node->assign.value;
        TTNode *target = node->assign.target;
        if (target->kind == TT_VAR_REF && val->kind == TT_ADDRESS_OF) {
            int32_t addr_idx = ctx->addr_count - 1;
            if (addr_idx >= 0 && *ctx->addr_nodes[addr_idx] == val) {
                ea_add_taint(ctx, target->var_ref.sym, addr_idx);
            }
        }
        if (target->kind == TT_VAR_REF && val->kind == TT_VAR_REF) {
            int32_t idx;
            if (ea_is_tainted(ctx, val->var_ref.sym, &idx)) {
                ea_add_taint(ctx, target->var_ref.sym, idx);
            }
        }
    }
}

// ── Phase 2: mark escaping poss ──

static void ea_check_escape(EscapeCtx *ctx, TTNode *node);

static void ea_check_visitor(void *raw_ctx, TTNode **child_ptr) {
    ea_check_escape((EscapeCtx *)raw_ctx, *child_ptr);
}

static void ea_check_escape(EscapeCtx *ctx, TTNode *node) {
    if (node == NULL) {
        return;
    }

    // A return stmt's value is in escaping pos.
    if (node->kind == TT_RETURN && node->return_stmt.value != NULL) {
        ea_mark_escaping(ctx, node->return_stmt.value);
    }

    // Walk children (skip nested fn decls).
    if (node->kind != TT_FN_DECL) {
        tt_visit_children(node, ea_check_visitor, ctx);
    }
}

// ── Phase 3: promote escaping TT_ADDRESS_OF → TT_HEAP_ALLOC ──

static void ea_promote(EscapeCtx *ctx) {
    for (int32_t i = 0; i < ctx->addr_count; i++) {
        if (!ctx->promote[i]) {
            continue;
        }
        TTNode *old = *ctx->addr_nodes[i];
        TTNode *heap = tt_new(ctx->arena, TT_HEAP_ALLOC, old->type, old->loc);
        heap->heap_alloc.operand = old->address_of.operand;
        *ctx->addr_nodes[i] = heap;
    }
}

// ── Fn-level driver ──

static void ea_analyse_fn(Arena *arena, TTNode *func) {
    TTNode *body = func->fn_decl.body;
    if (body == NULL) {
        return;
    }

    EscapeCtx ctx = {0};
    ctx.arena = arena;

    // Phase 1: collect TT_ADDRESS_OF nodes and taint map.
    ea_collect(&ctx, &body);

    // The fn body's block result is also in escaping pos
    // (it becomes the return value for expr-bodied fns).
    if (body->kind == TT_BLOCK && body->block.result != NULL) {
        ea_mark_escaping(&ctx, body->block.result);
    }

    // Phase 2: find return stmts and mark their values as escaping.
    ea_check_escape(&ctx, body);

    // Phase 3: replace escaping TT_ADDRESS_OF with TT_HEAP_ALLOC.
    ea_promote(&ctx);
}

// ── Top-level pass ──

static void escape_walk_file(Arena *arena, TTNode *node) {
    if (node == NULL) {
        return;
    }
    if (node->kind == TT_FN_DECL) {
        ea_analyse_fn(arena, node);
        return;
    }
    if (node->kind == TT_FILE) {
        for (int32_t i = 0; i < BUF_LEN(node->file.decls); i++) {
            escape_walk_file(arena, node->file.decls[i]);
        }
    }
}

void tt_pass_escape_analysis(Arena *arena, TTNode *root) {
    escape_walk_file(arena, root);
}
