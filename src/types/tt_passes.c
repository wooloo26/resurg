#include "types/tt_passes.h"

#include <string.h>

// ── Constant folding ──────────────────────────────────────────────────

/** Try folding a binary op on two integer literals. Returns a new node or NULL. */
static TtNode *fold_int_binary(Arena *arena, const TtNode *node) {
    const TtNode *left = node->binary.left;
    const TtNode *right = node->binary.right;

    if (left->kind != TT_INT_LITERAL || right->kind != TT_INT_LITERAL) {
        return NULL;
    }
    if (left->int_literal.int_kind != right->int_literal.int_kind) {
        return NULL;
    }

    TypeKind kind = left->int_literal.int_kind;
    int64_t lv = (int64_t)left->int_literal.value;
    int64_t rv = (int64_t)right->int_literal.value;
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

    TtNode *lit = tt_new(arena, TT_INT_LITERAL, node->type, node->location);
    lit->int_literal.value = (uint64_t)result;
    lit->int_literal.int_kind = kind;
    return lit;
}

/** Try folding a binary op on two float literals. Returns a new node or NULL. */
static TtNode *fold_float_binary(Arena *arena, const TtNode *node) {
    const TtNode *left = node->binary.left;
    const TtNode *right = node->binary.right;

    if (left->kind != TT_FLOAT_LITERAL || right->kind != TT_FLOAT_LITERAL) {
        return NULL;
    }
    if (left->float_literal.float_kind != right->float_literal.float_kind) {
        return NULL;
    }

    TypeKind kind = left->float_literal.float_kind;
    double lv = left->float_literal.value;
    double rv = right->float_literal.value;
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

    TtNode *lit = tt_new(arena, TT_FLOAT_LITERAL, node->type, node->location);
    lit->float_literal.value = result;
    lit->float_literal.float_kind = kind;
    return lit;
}

/** Try folding a unary negation on an integer or float literal. */
static TtNode *fold_unary_negate(Arena *arena, const TtNode *node) {
    if (node->unary.op != TOKEN_MINUS) {
        return NULL;
    }
    const TtNode *operand = node->unary.operand;
    if (operand->kind == TT_INT_LITERAL) {
        TtNode *lit = tt_new(arena, TT_INT_LITERAL, node->type, node->location);
        lit->int_literal.value = (uint64_t)(-(int64_t)operand->int_literal.value);
        lit->int_literal.int_kind = operand->int_literal.int_kind;
        return lit;
    }
    if (operand->kind == TT_FLOAT_LITERAL) {
        TtNode *lit = tt_new(arena, TT_FLOAT_LITERAL, node->type, node->location);
        lit->float_literal.value = -operand->float_literal.value;
        lit->float_literal.float_kind = operand->float_literal.float_kind;
        return lit;
    }
    return NULL;
}

static void const_fold(Arena *arena, TtNode **node_ptr);

/** Visitor adapter: fold constants in each child node. */
static void const_fold_visitor(void *ctx, TtNode **child_ptr) {
    const_fold((Arena *)ctx, child_ptr);
}

/** Recursively fold constants in @p node_ptr, replacing in-place. */
static void const_fold(Arena *arena, TtNode **node_ptr) {
    TtNode *node = *node_ptr;
    if (node == NULL) {
        return;
    }

    // Walk children first (bottom-up folding)
    tt_visit_children(node, const_fold_visitor, arena);

    // Attempt folding at this node
    TtNode *folded = NULL;
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

void tt_pass_const_fold(Arena *arena, TtNode *root) {
    const_fold(arena, &root);
}

// ── Escape analysis ───────────────────────────────────────────────────

/**
 * Per-function escape state: tracks which TtSymbol pointers hold the
 * result of a TT_ADDRESS_OF, and which TT_ADDRESS_OF nodes escape.
 */

/** Maximum number of address-of nodes tracked per function. */
#define EA_MAX_ADDR 256

/** Maximum number of tainted symbols (variables holding address-of results). */
#define EA_MAX_TAINT 256

typedef struct {
    Arena *arena;

    // All TT_ADDRESS_OF node pointers found in the current function body.
    TtNode **addr_nodes[EA_MAX_ADDR];
    int32_t addr_count;

    // Symbols tainted with an address-of result (TtSymbol pointers).
    const TtSymbol *tainted[EA_MAX_TAINT];
    // For each tainted symbol, the index into addr_nodes of the feeding
    // TT_ADDRESS_OF (so we can promote the right one).
    int32_t tainted_addr_idx[EA_MAX_TAINT];
    int32_t taint_count;

    // Bitmap: which addr_nodes entries must be promoted.
    bool promote[EA_MAX_ADDR];
} EscapeCtx;

/** Return true if @p sym is in the tainted set; write its addr index to @p out_idx. */
static bool ea_is_tainted(const EscapeCtx *ctx, const TtSymbol *sym, int32_t *out_idx) {
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

/** Record a tainted symbol ← address-of mapping. */
static void ea_add_taint(EscapeCtx *ctx, const TtSymbol *sym, int32_t addr_idx) {
    if (ctx->taint_count < EA_MAX_TAINT) {
        ctx->tainted[ctx->taint_count] = sym;
        ctx->tainted_addr_idx[ctx->taint_count] = addr_idx;
        ctx->taint_count++;
    }
}

/** Record a TT_ADDRESS_OF node pointer for later promotion. */
static int32_t ea_add_addr(EscapeCtx *ctx, TtNode **node_ptr) {
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
 * Mark the expression at @p node as escaping.  If it is a direct
 * TT_ADDRESS_OF or a variable reference to a tainted symbol,
 * flag the corresponding address-of node for heap promotion.
 */
static void ea_mark_escaping(EscapeCtx *ctx, const TtNode *node) {
    if (node == NULL) {
        return;
    }
    if (node->kind == TT_ADDRESS_OF) {
        // Direct address-of in escaping position — find it in addr list.
        for (int32_t i = 0; i < ctx->addr_count; i++) {
            if (*ctx->addr_nodes[i] == node) {
                ctx->promote[i] = true;
                return;
            }
        }
        return;
    }
    if (node->kind == TT_VARIABLE_REFERENCE) {
        int32_t idx;
        if (ea_is_tainted(ctx, node->variable_reference.symbol, &idx)) {
            ctx->promote[idx] = true;
        }
        return;
    }
    // For if-expression results, both branches may escape.
    if (node->kind == TT_IF) {
        ea_mark_escaping(ctx, node->if_expression.then_body);
        ea_mark_escaping(ctx, node->if_expression.else_body);
        return;
    }
    // For block results, the result expression escapes.
    if (node->kind == TT_BLOCK && node->block.result != NULL) {
        ea_mark_escaping(ctx, node->block.result);
        return;
    }
    // For match expressions, each arm body may escape.
    if (node->kind == TT_MATCH) {
        for (int32_t i = 0; i < BUFFER_LENGTH(node->match_expr.arm_bodies); i++) {
            ea_mark_escaping(ctx, node->match_expr.arm_bodies[i]);
        }
        return;
    }
}

// ── Phase 1: collect address-of nodes and build taint map ──

static void ea_collect(EscapeCtx *ctx, TtNode **node_ptr);

static void ea_collect_visitor(void *raw_ctx, TtNode **child_ptr) {
    ea_collect((EscapeCtx *)raw_ctx, child_ptr);
}

static void ea_collect(EscapeCtx *ctx, TtNode **node_ptr) {
    TtNode *node = *node_ptr;
    if (node == NULL) {
        return;
    }

    // Walk children first (bottom-up) so inner ADDRESS_OF nodes are
    // recorded before the enclosing declaration tries to reference them.
    // Skip nested function declarations — they have their own scopes.
    if (node->kind != TT_FUNCTION_DECLARATION) {
        tt_visit_children(node, ea_collect_visitor, ctx);
    }

    // Record every TT_ADDRESS_OF.
    if (node->kind == TT_ADDRESS_OF) {
        ea_add_addr(ctx, node_ptr);
    }

    // If a variable is initialised with a TT_ADDRESS_OF, taint the symbol.
    if (node->kind == TT_VARIABLE_DECLARATION && node->variable_declaration.initializer != NULL) {
        TtNode *init = node->variable_declaration.initializer;
        if (init->kind == TT_ADDRESS_OF) {
            int32_t addr_idx = ctx->addr_count - 1;
            if (addr_idx >= 0 && *ctx->addr_nodes[addr_idx] == init) {
                ea_add_taint(ctx, node->variable_declaration.symbol, addr_idx);
            }
        }
    }

    // If a variable is assigned a TT_ADDRESS_OF or tainted value, taint it.
    if (node->kind == TT_ASSIGN) {
        TtNode *val = node->assign.value;
        TtNode *target = node->assign.target;
        if (target->kind == TT_VARIABLE_REFERENCE && val->kind == TT_ADDRESS_OF) {
            int32_t addr_idx = ctx->addr_count - 1;
            if (addr_idx >= 0 && *ctx->addr_nodes[addr_idx] == val) {
                ea_add_taint(ctx, target->variable_reference.symbol, addr_idx);
            }
        }
        if (target->kind == TT_VARIABLE_REFERENCE && val->kind == TT_VARIABLE_REFERENCE) {
            int32_t idx;
            if (ea_is_tainted(ctx, val->variable_reference.symbol, &idx)) {
                ea_add_taint(ctx, target->variable_reference.symbol, idx);
            }
        }
    }
}

// ── Phase 2: mark escaping positions ──

static void ea_check_escape(EscapeCtx *ctx, TtNode *node);

static void ea_check_visitor(void *raw_ctx, TtNode **child_ptr) {
    ea_check_escape((EscapeCtx *)raw_ctx, *child_ptr);
}

static void ea_check_escape(EscapeCtx *ctx, TtNode *node) {
    if (node == NULL) {
        return;
    }

    // A return statement's value is in escaping position.
    if (node->kind == TT_RETURN && node->return_statement.value != NULL) {
        ea_mark_escaping(ctx, node->return_statement.value);
    }

    // Walk children (skip nested function declarations).
    if (node->kind != TT_FUNCTION_DECLARATION) {
        tt_visit_children(node, ea_check_visitor, ctx);
    }
}

// ── Phase 3: promote escaping TT_ADDRESS_OF → TT_HEAP_ALLOC ──

static void ea_promote(EscapeCtx *ctx) {
    for (int32_t i = 0; i < ctx->addr_count; i++) {
        if (!ctx->promote[i]) {
            continue;
        }
        TtNode *old = *ctx->addr_nodes[i];
        TtNode *heap = tt_new(ctx->arena, TT_HEAP_ALLOC, old->type, old->location);
        heap->heap_alloc.operand = old->address_of.operand;
        *ctx->addr_nodes[i] = heap;
    }
}

// ── Function-level driver ──

static void ea_analyse_function(Arena *arena, TtNode *func) {
    TtNode *body = func->function_declaration.body;
    if (body == NULL) {
        return;
    }

    EscapeCtx ctx = {0};
    ctx.arena = arena;

    // Phase 1: collect TT_ADDRESS_OF nodes and taint map.
    ea_collect(&ctx, &body);

    // The function body's block result is also in escaping position
    // (it becomes the return value for expression-bodied functions).
    if (body->kind == TT_BLOCK && body->block.result != NULL) {
        ea_mark_escaping(&ctx, body->block.result);
    }

    // Phase 2: find return statements and mark their values as escaping.
    ea_check_escape(&ctx, body);

    // Phase 3: replace escaping TT_ADDRESS_OF with TT_HEAP_ALLOC.
    ea_promote(&ctx);
}

// ── Top-level pass ──

static void escape_walk_file(Arena *arena, TtNode *node) {
    if (node == NULL) {
        return;
    }
    if (node->kind == TT_FUNCTION_DECLARATION) {
        ea_analyse_function(arena, node);
        return;
    }
    if (node->kind == TT_FILE) {
        for (int32_t i = 0; i < BUFFER_LENGTH(node->file.declarations); i++) {
            escape_walk_file(arena, node->file.declarations[i]);
        }
    }
}

void tt_pass_escape_analysis(Arena *arena, TtNode *root) {
    escape_walk_file(arena, root);
}
