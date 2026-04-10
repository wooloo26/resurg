#include <string.h>

#include "hir_passes.h"

/**
 * @file hir_escape.c
 * @brief HIR escape-analysis pass — promote escaping address-of to heap alloc.
 *
 * Three-phase algorithm per fn:
 *   1. Collect all HIR_ADDRESS_OF nodes and build a taint map.
 *   2. Mark return/result positions as escaping.
 *   3. Replace escaping HIR_ADDRESS_OF with HIR_HEAP_ALLOC.
 */

// ── Per-fn escape context ─────────────────────────────────────────────

/** Maximum number of address-of nodes tracked per fn. */
#define EA_MAX_ADDR 256

/** Maximum number of tainted syms (vars holding address-of results). */
#define EA_MAX_TAINT 256

typedef struct {
    Arena *arena;

    // All HIR_ADDRESS_OF node ptrs found in the current fn body.
    HirNode **addr_nodes[EA_MAX_ADDR];
    int32_t addr_count;

    // Syms tainted with an address-of result (HirSym ptrs).
    const HirSym *tainted[EA_MAX_TAINT];
    // For each tainted sym, the idx into addr_nodes of the feeding
    // HIR_ADDRESS_OF (so we can promote the right one).
    int32_t tainted_addr_idx[EA_MAX_TAINT];
    int32_t taint_count;

    // Bitmap: which addr_nodes entries must be promoted.
    bool promote[EA_MAX_ADDR];
} EscapeCtx;

// ── Taint helpers ─────────────────────────────────────────────────────

/** Return true if @p sym is in the tainted set; write its addr idx to @p out_idx. */
static bool ea_is_tainted(const EscapeCtx *ctx, const HirSym *sym, int32_t *out_idx) {
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
static void ea_add_taint(EscapeCtx *ctx, const HirSym *sym, int32_t addr_idx) {
    if (ctx->taint_count < EA_MAX_TAINT) {
        ctx->tainted[ctx->taint_count] = sym;
        ctx->tainted_addr_idx[ctx->taint_count] = addr_idx;
        ctx->taint_count++;
    }
}

/** Record a HIR_ADDRESS_OF node ptr for later promotion. */
static int32_t ea_add_addr(EscapeCtx *ctx, HirNode **node_ptr) {
    if (ctx->addr_count < EA_MAX_ADDR) {
        int32_t idx = ctx->addr_count;
        ctx->addr_nodes[idx] = node_ptr;
        ctx->promote[idx] = false;
        ctx->addr_count++;
        return idx;
    }
    return -1;
}

// ── Escape marking ────────────────────────────────────────────────────

/**
 * Mark the expr at @p node as escaping.  If it is a direct
 * HIR_ADDRESS_OF or a var ref to a tainted sym,
 * flag the corresponding address-of node for heap promotion.
 */
static void ea_mark_escaping(EscapeCtx *ctx, const HirNode *node) {
    if (node == NULL) {
        return;
    }
    if (node->kind == HIR_ADDRESS_OF) {
        // Direct address-of in escaping pos — find it in addr list.
        for (int32_t i = 0; i < ctx->addr_count; i++) {
            if (*ctx->addr_nodes[i] == node) {
                ctx->promote[i] = true;
                return;
            }
        }
        return;
    }
    if (node->kind == HIR_VAR_REF) {
        int32_t idx;
        if (ea_is_tainted(ctx, node->var_ref.sym, &idx)) {
            ctx->promote[idx] = true;
        }
        return;
    }
    // For if-expr results, both branches may escape.
    if (node->kind == HIR_IF) {
        ea_mark_escaping(ctx, node->if_expr.then_body);
        ea_mark_escaping(ctx, node->if_expr.else_body);
        return;
    }
    // For block results, the result expr escapes.
    if (node->kind == HIR_BLOCK && node->block.result != NULL) {
        ea_mark_escaping(ctx, node->block.result);
        return;
    }
    // For match exprs, each arm body may escape.
    if (node->kind == HIR_MATCH) {
        for (int32_t i = 0; i < BUF_LEN(node->match_expr.arms); i++) {
            ea_mark_escaping(ctx, node->match_expr.arms[i].body);
        }
        return;
    }
}

// ── Phase 1: collect address-of nodes and build taint map ──

static void ea_collect(EscapeCtx *ctx, HirNode **node_ptr);

static void ea_collect_visitor(void *raw_ctx, HirNode **child_ptr) {
    ea_collect((EscapeCtx *)raw_ctx, child_ptr);
}

static void ea_collect(EscapeCtx *ctx, HirNode **node_ptr) {
    HirNode *node = *node_ptr;
    if (node == NULL) {
        return;
    }

    // Walk children first (bottom-up) so inner ADDRESS_OF nodes are
    // recorded before the enclosing decl tries to ref them.
    // Skip nested fn decls — they have their own scopes.
    if (node->kind != HIR_FN_DECL) {
        hir_visit_children(node, ea_collect_visitor, ctx);
    }

    // Record every HIR_ADDRESS_OF.
    if (node->kind == HIR_ADDRESS_OF) {
        ea_add_addr(ctx, node_ptr);
    }

    // If a var is initialised with a HIR_ADDRESS_OF, taint the sym.
    if (node->kind == HIR_VAR_DECL && node->var_decl.init != NULL) {
        HirNode *init = node->var_decl.init;
        if (init->kind == HIR_ADDRESS_OF) {
            int32_t addr_idx = ctx->addr_count - 1;
            if (addr_idx >= 0 && *ctx->addr_nodes[addr_idx] == init) {
                ea_add_taint(ctx, node->var_decl.sym, addr_idx);
            }
        }
    }

    // If a var is assigned a HIR_ADDRESS_OF or tainted value, taint it.
    if (node->kind == HIR_ASSIGN) {
        HirNode *val = node->assign.value;
        HirNode *target = node->assign.target;
        if (target->kind == HIR_VAR_REF && val->kind == HIR_ADDRESS_OF) {
            int32_t addr_idx = ctx->addr_count - 1;
            if (addr_idx >= 0 && *ctx->addr_nodes[addr_idx] == val) {
                ea_add_taint(ctx, target->var_ref.sym, addr_idx);
            }
        }
        if (target->kind == HIR_VAR_REF && val->kind == HIR_VAR_REF) {
            int32_t idx;
            if (ea_is_tainted(ctx, val->var_ref.sym, &idx)) {
                ea_add_taint(ctx, target->var_ref.sym, idx);
            }
        }
    }
}

// ── Phase 2: mark escaping positions ──

static void ea_check_escape(EscapeCtx *ctx, HirNode *node);

static void ea_check_visitor(void *raw_ctx, HirNode **child_ptr) {
    ea_check_escape((EscapeCtx *)raw_ctx, *child_ptr);
}

static void ea_check_escape(EscapeCtx *ctx, HirNode *node) {
    if (node == NULL) {
        return;
    }

    // A return stmt's value is in escaping pos.
    if (node->kind == HIR_RETURN && node->return_stmt.value != NULL) {
        ea_mark_escaping(ctx, node->return_stmt.value);
    }

    // Walk children (skip nested fn decls).
    if (node->kind != HIR_FN_DECL) {
        hir_visit_children(node, ea_check_visitor, ctx);
    }
}

// ── Phase 3: promote escaping HIR_ADDRESS_OF → HIR_HEAP_ALLOC ──

static void ea_promote(EscapeCtx *ctx) {
    for (int32_t i = 0; i < ctx->addr_count; i++) {
        if (!ctx->promote[i]) {
            continue;
        }
        HirNode *old = *ctx->addr_nodes[i];
        HirNode *heap = hir_new(ctx->arena, HIR_HEAP_ALLOC, old->type, old->loc);
        heap->heap_alloc.operand = old->address_of.operand;
        *ctx->addr_nodes[i] = heap;
    }
}

// ── Fn-level driver ──

static void ea_analyse_fn(Arena *arena, HirNode *func) {
    HirNode *body = func->fn_decl.body;
    if (body == NULL) {
        return;
    }

    EscapeCtx ctx = {0};
    ctx.arena = arena;

    // Phase 1: collect HIR_ADDRESS_OF nodes and taint map.
    ea_collect(&ctx, &body);

    // The fn body's block result is also in escaping pos
    // (it becomes the return value for expr-bodied fns).
    if (body->kind == HIR_BLOCK && body->block.result != NULL) {
        ea_mark_escaping(&ctx, body->block.result);
    }

    // Phase 2: find return stmts and mark their values as escaping.
    ea_check_escape(&ctx, body);

    // Phase 3: replace escaping HIR_ADDRESS_OF with HIR_HEAP_ALLOC.
    ea_promote(&ctx);
}

// ── Top-level pass ──

static void escape_walk_file(Arena *arena, HirNode *node) {
    if (node == NULL) {
        return;
    }
    if (node->kind == HIR_FN_DECL) {
        ea_analyse_fn(arena, node);
        return;
    }
    if (node->kind == HIR_FILE) {
        for (int32_t i = 0; i < BUF_LEN(node->file.decls); i++) {
            escape_walk_file(arena, node->file.decls[i]);
        }
    }
}

void hir_pass_escape_analysis(Arena *arena, HirNode *root) {
    escape_walk_file(arena, root);
}
