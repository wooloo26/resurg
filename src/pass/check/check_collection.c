#include "_check.h"

// ── Collection literal checkers ────────────────────────────────────────

/** Infer and unify elem types across all elems in a collection lit. */
static const Type *check_elem_list(Sema *sema, ASTNode **elems, ASTType *ast_elem_type) {
    const Type *elem_type = resolve_ast_type(sema, ast_elem_type);
    for (int32_t i = 0; i < BUF_LEN(elems); i++) {
        const Type *elem = check_node(sema, elems[i]);
        if (elem_type == NULL && elem != NULL) {
            elem_type = elem;
        }
        if (elem_type != NULL) {
            promote_lit(elems[i], elem_type);
        }
    }
    if (elem_type == NULL) {
        elem_type = &TYPE_I32_INST;
    }
    return elem_type;
}

const Type *check_array_lit(Sema *sema, ASTNode *node) {
    const Type *elem_type =
        check_elem_list(sema, node->array_lit.elems, &node->array_lit.elem_type);
    int32_t size = node->array_lit.size;
    if (size == 0) {
        size = BUF_LEN(node->array_lit.elems);
    }
    return type_create_array(sema->base.arena, elem_type, size);
}

const Type *check_slice_lit(Sema *sema, ASTNode *node) {
    const Type *elem_type =
        check_elem_list(sema, node->slice_lit.elems, &node->slice_lit.elem_type);
    return type_create_slice(sema->base.arena, elem_type);
}

const Type *check_slice_expr(Sema *sema, ASTNode *node) {
    const Type *object_type = check_node(sema, node->slice_expr.object);
    if (node->slice_expr.start != NULL) {
        check_node(sema, node->slice_expr.start);
    }
    if (node->slice_expr.end != NULL) {
        check_node(sema, node->slice_expr.end);
    }
    // Auto-deref: unwrap *[]T / *[N]T
    if (object_type != NULL && object_type->kind == TYPE_PTR) {
        object_type = object_type->ptr.pointee;
    }
    if (object_type != NULL && object_type->kind == TYPE_ARRAY) {
        return type_create_slice(sema->base.arena, object_type->array.elem);
    }
    if (object_type != NULL && object_type->kind == TYPE_SLICE) {
        return type_create_slice(sema->base.arena, object_type->slice.elem);
    }
    return &TYPE_ERR_INST;
}

const Type *check_tuple_lit(Sema *sema, ASTNode *node) {
    const Type ** /* buf */ elem_types = NULL;
    for (int32_t i = 0; i < BUF_LEN(node->tuple_lit.elems); i++) {
        const Type *elem = check_node(sema, node->tuple_lit.elems[i]);
        BUF_PUSH(elem_types, elem);
    }
    const Type *result = type_create_tuple(sema->base.arena, elem_types, BUF_LEN(elem_types));
    return result;
}
