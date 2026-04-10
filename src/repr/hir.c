#include "repr/hir.h"

// ── HirSym ───────────────────────────────────────────────────────────

const char *hir_sym_name(const HirSym *sym) {
    assert(sym != NULL);
    return sym->name;
}

const Type *hir_sym_type(const HirSym *sym) {
    assert(sym != NULL);
    return sym->type;
}

HirSym *hir_sym_new(Arena *arena, const HirSymSpec *spec) {
    HirSym *sym = arena_alloc_zero(arena, sizeof(HirSym));
    sym->kind = spec->kind;
    sym->name = spec->name;
    sym->type = spec->type;
    sym->is_mut = spec->is_mut;
    sym->loc = spec->loc;
    return sym;
}

// ── HirNode constructors ───────────────────────────────────────────────

HirNode *hir_new(Arena *arena, HirNodeKind kind, const Type *type, SrcLoc loc) {
    HirNode *node = arena_alloc_zero(arena, sizeof(HirNode));
    node->kind = kind;
    node->type = type != NULL ? type : &TYPE_UNIT_INST;
    node->loc = loc;
    return node;
}

// ── HIR child visitor ─────────────────────────────────────────────────

static void visit_buf(HirChildVisitor visitor, void *ctx, HirNode **buf, int32_t count) {
    for (int32_t i = 0; i < count; i++) {
        visitor(ctx, &buf[i]);
    }
}

void hir_visit_children(HirNode *node, HirChildVisitor visitor, void *ctx) {
    if (node == NULL) {
        return;
    }
    switch (node->kind) {
    case HIR_FILE:
        visit_buf(visitor, ctx, node->file.decls, BUF_LEN(node->file.decls));
        break;
    case HIR_FN_DECL:
        visit_buf(visitor, ctx, node->fn_decl.params, BUF_LEN(node->fn_decl.params));
        if (node->fn_decl.body != NULL) {
            visitor(ctx, &node->fn_decl.body);
        }
        break;
    case HIR_BLOCK:
        visit_buf(visitor, ctx, node->block.stmts, BUF_LEN(node->block.stmts));
        if (node->block.result != NULL) {
            visitor(ctx, &node->block.result);
        }
        break;
    case HIR_VAR_DECL:
        if (node->var_decl.init != NULL) {
            visitor(ctx, &node->var_decl.init);
        }
        break;
    case HIR_RETURN:
        if (node->return_stmt.value != NULL) {
            visitor(ctx, &node->return_stmt.value);
        }
        break;
    case HIR_ASSIGN:
        visitor(ctx, &node->assign.target);
        visitor(ctx, &node->assign.value);
        break;
    case HIR_BINARY:
        visitor(ctx, &node->binary.left);
        visitor(ctx, &node->binary.right);
        break;
    case HIR_UNARY:
        visitor(ctx, &node->unary.operand);
        break;
    case HIR_CALL:
        visitor(ctx, &node->call.callee);
        visit_buf(visitor, ctx, node->call.args, BUF_LEN(node->call.args));
        break;
    case HIR_IF:
        visitor(ctx, &node->if_expr.cond);
        visitor(ctx, &node->if_expr.then_body);
        if (node->if_expr.else_body != NULL) {
            visitor(ctx, &node->if_expr.else_body);
        }
        break;
    case HIR_ARRAY_LIT:
        visit_buf(visitor, ctx, node->array_lit.elems, BUF_LEN(node->array_lit.elems));
        break;
    case HIR_SLICE_LIT:
        visit_buf(visitor, ctx, node->slice_lit.elems, BUF_LEN(node->slice_lit.elems));
        break;
    case HIR_TUPLE_LIT:
        visit_buf(visitor, ctx, node->tuple_lit.elems, BUF_LEN(node->tuple_lit.elems));
        break;
    case HIR_IDX:
        visitor(ctx, &node->idx_access.object);
        visitor(ctx, &node->idx_access.idx);
        break;
    case HIR_SLICE_EXPR:
        visitor(ctx, &node->slice_expr.object);
        if (node->slice_expr.start != NULL) {
            visitor(ctx, &node->slice_expr.start);
        }
        if (node->slice_expr.end != NULL) {
            visitor(ctx, &node->slice_expr.end);
        }
        break;
    case HIR_TUPLE_IDX:
        visitor(ctx, &node->tuple_idx.object);
        break;
    case HIR_TYPE_CONVERSION:
        visitor(ctx, &node->type_conversion.operand);
        break;
    case HIR_MODULE_ACCESS:
        visitor(ctx, &node->module_access.object);
        break;
    case HIR_LOOP:
        visitor(ctx, &node->loop.body);
        break;
    case HIR_DEFER:
        visitor(ctx, &node->defer_stmt.body);
        break;
    case HIR_STRUCT_DECL:
        break;
    case HIR_STRUCT_LIT:
        visit_buf(visitor, ctx, node->struct_lit.field_values,
                  BUF_LEN(node->struct_lit.field_values));
        break;
    case HIR_STRUCT_FIELD_ACCESS:
        visitor(ctx, &node->struct_field_access.object);
        break;
    case HIR_METHOD_CALL:
        visitor(ctx, &node->method_call.recv);
        visit_buf(visitor, ctx, node->method_call.args, BUF_LEN(node->method_call.args));
        break;
    case HIR_HEAP_ALLOC:
        visitor(ctx, &node->heap_alloc.operand);
        break;
    case HIR_ADDRESS_OF:
        visitor(ctx, &node->address_of.operand);
        break;
    case HIR_DEREF:
        visitor(ctx, &node->deref.operand);
        break;
    case HIR_ENUM_DECL:
        break;
    case HIR_MATCH:
        visitor(ctx, &node->match_expr.operand);
        for (int32_t i = 0; i < BUF_LEN(node->match_expr.arms); i++) {
            HirMatchArm *arm = &node->match_expr.arms[i];
            if (arm->cond != NULL) {
                visitor(ctx, &arm->cond);
            }
            if (arm->guard != NULL) {
                visitor(ctx, &arm->guard);
            }
            if (arm->body != NULL) {
                visitor(ctx, &arm->body);
            }
            if (arm->bindings != NULL) {
                visitor(ctx, &arm->bindings);
            }
        }
        break;
    case HIR_CLOSURE:
        visit_buf(visitor, ctx, node->closure.params, BUF_LEN(node->closure.params));
        visitor(ctx, &node->closure.body);
        break;
    default:
        break;
    }
}
