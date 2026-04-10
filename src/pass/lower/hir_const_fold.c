#include "hir_passes.h"

/**
 * @file hir_const_fold.c
 * @brief HIR constant-folding pass — fold integer and float lit operations.
 */

// ── Integer folding ───────────────────────────────────────────────────

/** Try folding a binary op on two integer lits. Returns a new node or NULL. */
static HirNode *fold_int_binary(Arena *arena, const HirNode *node) {
    const HirNode *left = node->binary.left;
    const HirNode *right = node->binary.right;

    if (left->kind != HIR_INT_LIT || right->kind != HIR_INT_LIT) {
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

    HirNode *lit = hir_new(arena, HIR_INT_LIT, node->type, node->loc);
    lit->int_lit.value = (uint64_t)result;
    lit->int_lit.int_kind = kind;
    return lit;
}

// ── Float folding ─────────────────────────────────────────────────────

/** Try folding a binary op on two float lits. Returns a new node or NULL. */
static HirNode *fold_float_binary(Arena *arena, const HirNode *node) {
    const HirNode *left = node->binary.left;
    const HirNode *right = node->binary.right;

    if (left->kind != HIR_FLOAT_LIT || right->kind != HIR_FLOAT_LIT) {
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

    HirNode *lit = hir_new(arena, HIR_FLOAT_LIT, node->type, node->loc);
    lit->float_lit.value = result;
    lit->float_lit.float_kind = kind;
    return lit;
}

// ── Unary folding ─────────────────────────────────────────────────────

/** Try folding a unary negation on an integer or float lit. */
static HirNode *fold_unary_negate(Arena *arena, const HirNode *node) {
    if (node->unary.op != TOKEN_MINUS) {
        return NULL;
    }
    const HirNode *operand = node->unary.operand;
    if (operand->kind == HIR_INT_LIT) {
        HirNode *lit = hir_new(arena, HIR_INT_LIT, node->type, node->loc);
        lit->int_lit.value = (uint64_t)(-(int64_t)operand->int_lit.value);
        lit->int_lit.int_kind = operand->int_lit.int_kind;
        return lit;
    }
    if (operand->kind == HIR_FLOAT_LIT) {
        HirNode *lit = hir_new(arena, HIR_FLOAT_LIT, node->type, node->loc);
        lit->float_lit.value = -operand->float_lit.value;
        lit->float_lit.float_kind = operand->float_lit.float_kind;
        return lit;
    }
    return NULL;
}

// ── Recursive walk ────────────────────────────────────────────────────

static void const_fold(Arena *arena, HirNode **node_ptr);

/** Visitor adapter: fold constants in each child node. */
static void const_fold_visitor(void *ctx, HirNode **child_ptr) {
    const_fold((Arena *)ctx, child_ptr);
}

/** Recursively fold constants in @p node_ptr, replacing in-place. */
static void const_fold(Arena *arena, HirNode **node_ptr) {
    HirNode *node = *node_ptr;
    if (node == NULL) {
        return;
    }

    // Walk children first (bottom-up folding)
    hir_visit_children(node, const_fold_visitor, arena);

    // Attempt folding at this node
    HirNode *folded = NULL;
    if (node->kind == HIR_BINARY) {
        folded = fold_int_binary(arena, node);
        if (folded == NULL) {
            folded = fold_float_binary(arena, node);
        }
    } else if (node->kind == HIR_UNARY) {
        folded = fold_unary_negate(arena, node);
    }
    if (folded != NULL) {
        *node_ptr = folded;
    }
}

void hir_pass_const_fold(Arena *arena, HirNode *root) {
    const_fold(arena, &root);
}
