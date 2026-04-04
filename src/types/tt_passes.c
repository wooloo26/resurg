#include "types/tt_passes.h"

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
