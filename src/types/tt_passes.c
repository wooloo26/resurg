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

/** Try folding a unary negation on an integer literal. */
static TtNode *fold_unary_negate_int(Arena *arena, const TtNode *node) {
    if (node->unary.op != TOKEN_MINUS) {
        return NULL;
    }
    const TtNode *operand = node->unary.operand;
    if (operand->kind != TT_INT_LITERAL) {
        return NULL;
    }

    TtNode *lit = tt_new(arena, TT_INT_LITERAL, node->type, node->location);
    lit->int_literal.value = (uint64_t)(-(int64_t)operand->int_literal.value);
    lit->int_literal.int_kind = operand->int_literal.int_kind;
    return lit;
}

/** Try folding a unary negation on a float literal. */
static TtNode *fold_unary_negate_float(Arena *arena, const TtNode *node) {
    if (node->unary.op != TOKEN_MINUS) {
        return NULL;
    }
    const TtNode *operand = node->unary.operand;
    if (operand->kind != TT_FLOAT_LITERAL) {
        return NULL;
    }

    TtNode *lit = tt_new(arena, TT_FLOAT_LITERAL, node->type, node->location);
    lit->float_literal.value = -operand->float_literal.value;
    lit->float_literal.float_kind = operand->float_literal.float_kind;
    return lit;
}

static void const_fold_children(Arena *arena, TtNode **children, int32_t count);

/** Recursively fold constants in @p node_ptr, replacing in-place. */
static void const_fold(Arena *arena, TtNode **node_ptr) {
    TtNode *node = *node_ptr;
    if (node == NULL) {
        return;
    }

    // Walk children first (bottom-up folding)
    switch (node->kind) {
    case TT_FILE:
        const_fold_children(arena, node->file.declarations, BUFFER_LENGTH(node->file.declarations));
        break;
    case TT_FUNCTION_DECLARATION:
        const_fold(arena, &node->function_declaration.body);
        break;
    case TT_BLOCK:
        const_fold_children(arena, node->block.statements, BUFFER_LENGTH(node->block.statements));
        const_fold(arena, &node->block.result);
        break;
    case TT_VARIABLE_DECLARATION:
        const_fold(arena, &node->variable_declaration.initializer);
        break;
    case TT_RETURN:
        const_fold(arena, &node->return_statement.value);
        break;
    case TT_ASSIGN:
        const_fold(arena, &node->assign.target);
        const_fold(arena, &node->assign.value);
        break;
    case TT_BINARY:
        const_fold(arena, &node->binary.left);
        const_fold(arena, &node->binary.right);
        break;
    case TT_UNARY:
        const_fold(arena, &node->unary.operand);
        break;
    case TT_CALL:
        const_fold(arena, &node->call.callee);
        const_fold_children(arena, node->call.arguments, BUFFER_LENGTH(node->call.arguments));
        break;
    case TT_IF:
        const_fold(arena, &node->if_expression.condition);
        const_fold(arena, &node->if_expression.then_body);
        const_fold(arena, &node->if_expression.else_body);
        break;
    case TT_ARRAY_LITERAL:
        const_fold_children(arena, node->array_literal.elements,
                            BUFFER_LENGTH(node->array_literal.elements));
        break;
    case TT_TUPLE_LITERAL:
        const_fold_children(arena, node->tuple_literal.elements,
                            BUFFER_LENGTH(node->tuple_literal.elements));
        break;
    case TT_INDEX:
        const_fold(arena, &node->index_access.object);
        const_fold(arena, &node->index_access.index);
        break;
    case TT_TUPLE_INDEX:
        const_fold(arena, &node->tuple_index.object);
        break;
    case TT_TYPE_CONVERSION:
        const_fold(arena, &node->type_conversion.operand);
        break;
    case TT_MODULE_ACCESS:
        const_fold(arena, &node->module_access.object);
        break;
    case TT_LOOP:
        const_fold(arena, &node->loop.body);
        break;
    default:
        break;
    }

    // Attempt folding at this node
    TtNode *folded = NULL;
    if (node->kind == TT_BINARY) {
        folded = fold_int_binary(arena, node);
        if (folded == NULL) {
            folded = fold_float_binary(arena, node);
        }
    } else if (node->kind == TT_UNARY) {
        folded = fold_unary_negate_int(arena, node);
        if (folded == NULL) {
            folded = fold_unary_negate_float(arena, node);
        }
    }
    if (folded != NULL) {
        *node_ptr = folded;
    }
}

static void const_fold_children(Arena *arena, TtNode **children, int32_t count) {
    for (int32_t i = 0; i < count; i++) {
        const_fold(arena, &children[i]);
    }
}

void tt_pass_const_fold(Arena *arena, TtNode *root) {
    const_fold(arena, &root);
}
