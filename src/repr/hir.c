#include "repr/hir.h"

#include "pass/check/_check.h"

// ── HirSym ───────────────────────────────────────────────────────────

const char *hir_sym_name(const HirSym *sym) {
    assert(sym != NULL && sym->sema_sym != NULL);
    return sym->sema_sym->name;
}

const Type *hir_sym_type(const HirSym *sym) {
    assert(sym != NULL && sym->sema_sym != NULL);
    return sym->sema_sym->type;
}

HirSym *hir_sym_new(Arena *arena, HirSymKind kind, Sym *sema_sym, bool is_mut, SrcLoc loc) {
    HirSym *sym = arena_alloc_zero(arena, sizeof(HirSym));
    sym->kind = kind;
    sym->sema_sym = sema_sym;
    sym->is_mut = is_mut;
    sym->loc = loc;
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

// ── TT dump ───────────────────────────────────────────────────────────

/** Print @p indent levels of whitespace to stderr. */
static void dump_indent(int32_t indent) {
    for (int32_t i = 0; i < indent; i++) {
        fprintf(stderr, "  ");
    }
}

/** Return a short str label for a HirNodeKind. */
static const char *hir_node_kind_str(HirNodeKind kind) {
    switch (kind) {
    case HIR_FILE:
        return "File";
    case HIR_MODULE:
        return "Module";
    case HIR_TYPE_ALIAS:
        return "TypeAlias";
    case HIR_FN_DECL:
        return "FnDecl";
    case HIR_PARAM:
        return "Param";
    case HIR_VAR_DECL:
        return "VarDecl";
    case HIR_RETURN:
        return "Return";
    case HIR_ASSIGN:
        return "Assign";
    case HIR_BREAK:
        return "Break";
    case HIR_CONTINUE:
        return "Continue";
    case HIR_DEFER:
        return "Defer";
    case HIR_BOOL_LIT:
        return "BoolLit";
    case HIR_INT_LIT:
        return "IntLit";
    case HIR_FLOAT_LIT:
        return "FloatLit";
    case HIR_CHAR_LIT:
        return "CharLit";
    case HIR_STR_LIT:
        return "StrLit";
    case HIR_UNIT_LIT:
        return "UnitLit";
    case HIR_ARRAY_LIT:
        return "ArrayLit";
    case HIR_SLICE_LIT:
        return "SliceLit";
    case HIR_TUPLE_LIT:
        return "TupleLit";
    case HIR_VAR_REF:
        return "VarRef";
    case HIR_MODULE_ACCESS:
        return "ModuleAccess";
    case HIR_IDX:
        return "Idx";
    case HIR_SLICE_EXPR:
        return "SliceExpr";
    case HIR_TUPLE_IDX:
        return "TupleIdx";
    case HIR_UNARY:
        return "Unary";
    case HIR_BINARY:
        return "Binary";
    case HIR_CALL:
        return "Call";
    case HIR_TYPE_CONVERSION:
        return "TypeConv";
    case HIR_IF:
        return "If";
    case HIR_BLOCK:
        return "Block";
    case HIR_LOOP:
        return "Loop";
    case HIR_STRUCT_DECL:
        return "StructDecl";
    case HIR_STRUCT_LIT:
        return "StructLit";
    case HIR_STRUCT_FIELD_ACCESS:
        return "StructFieldAccess";
    case HIR_METHOD_CALL:
        return "MethodCall";
    case HIR_HEAP_ALLOC:
        return "HeapAlloc";
    case HIR_ADDRESS_OF:
        return "AddressOf";
    case HIR_DEREF:
        return "Deref";
    case HIR_ENUM_DECL:
        return "EnumDecl";
    case HIR_MATCH:
        return "Match";
    }
    return "?";
}

static void hir_dump_children(HirNode **children, int32_t count, int32_t indent);

static void dump_lit(const HirNode *node) {
    switch (node->kind) {
    case HIR_BOOL_LIT:
        fprintf(stderr, " %s\n", node->bool_lit.value ? "true" : "false");
        break;
    case HIR_INT_LIT:
        fprintf(stderr, " %lu\n", (unsigned long)node->int_lit.value);
        break;
    case HIR_FLOAT_LIT:
        fprintf(stderr, " %g\n", node->float_lit.value);
        break;
    case HIR_CHAR_LIT:
        fprintf(stderr, " U+%04X\n", node->char_lit.value);
        break;
    case HIR_STR_LIT:
        fprintf(stderr, " \"%s\"\n", node->str_lit.value);
        break;
    default:
        fprintf(stderr, "\n");
        break;
    }
}

static void dump_unary_binary(const HirNode *node, int32_t indent) {
    if (node->kind == HIR_UNARY) {
        fprintf(stderr, " %s\n", token_kind_str(node->unary.op));
        hir_dump(node->unary.operand, indent + 1);
    } else {
        fprintf(stderr, " %s\n", token_kind_str(node->binary.op));
        hir_dump(node->binary.left, indent + 1);
        hir_dump(node->binary.right, indent + 1);
    }
}

static void dump_fn_decl(const HirNode *node, int32_t indent) {
    fprintf(stderr, " \"%s\"%s\n", node->fn_decl.name, node->fn_decl.is_pub ? " pub" : "");
    hir_dump_children(node->fn_decl.params, BUF_LEN(node->fn_decl.params), indent + 1);
    if (node->fn_decl.body != NULL) {
        hir_dump(node->fn_decl.body, indent + 1);
    }
}

static void dump_var_decl(const HirNode *node, int32_t indent) {
    fprintf(stderr, " \"%s\"%s\n", node->var_decl.name, node->var_decl.is_mut ? " mut" : "");
    if (node->var_decl.init != NULL) {
        hir_dump(node->var_decl.init, indent + 1);
    }
}

static void dump_block(const HirNode *node, int32_t indent) {
    fprintf(stderr, "\n");
    hir_dump_children(node->block.stmts, BUF_LEN(node->block.stmts), indent + 1);
    if (node->block.result != NULL) {
        dump_indent(indent + 1);
        fprintf(stderr, "result:\n");
        hir_dump(node->block.result, indent + 2);
    }
}

static void dump_match_node(const HirNode *node, int32_t indent) {
    fprintf(stderr, "\n");
    hir_dump(node->match_expr.operand, indent + 1);
    for (int32_t i = 0; i < BUF_LEN(node->match_expr.arm_bodies); i++) {
        dump_indent(indent + 1);
        fprintf(stderr, "arm %d:\n", i);
        if (node->match_expr.arm_conds[i] != NULL) {
            hir_dump(node->match_expr.arm_conds[i], indent + 2);
        }
        if (node->match_expr.arm_guards[i] != NULL) {
            dump_indent(indent + 2);
            fprintf(stderr, "guard:\n");
            hir_dump(node->match_expr.arm_guards[i], indent + 3);
        }
        hir_dump(node->match_expr.arm_bodies[i], indent + 2);
    }
}

static void dump_if_node(const HirNode *node, int32_t indent) {
    fprintf(stderr, "\n");
    hir_dump(node->if_expr.cond, indent + 1);
    hir_dump(node->if_expr.then_body, indent + 1);
    if (node->if_expr.else_body != NULL) {
        hir_dump(node->if_expr.else_body, indent + 1);
    }
}

void hir_dump(const HirNode *node, int32_t indent) {
    if (node == NULL) {
        dump_indent(indent);
        fprintf(stderr, "<null>\n");
        return;
    }

    dump_indent(indent);
    fprintf(stderr, "%s", hir_node_kind_str(node->kind));

    switch (node->kind) {
    case HIR_FILE:
        fprintf(stderr, "\n");
        hir_dump_children(node->file.decls, BUF_LEN(node->file.decls), indent + 1);
        break;

    case HIR_MODULE:
        fprintf(stderr, " \"%s\"\n", node->module.name);
        break;

    case HIR_TYPE_ALIAS:
        fprintf(stderr, " \"%s\"%s\n", node->type_alias.name,
                node->type_alias.is_pub ? " pub" : "");
        break;

    case HIR_FN_DECL:
        dump_fn_decl(node, indent);
        break;

    case HIR_PARAM:
        fprintf(stderr, " \"%s\"\n", node->param.name);
        break;

    case HIR_VAR_DECL:
        dump_var_decl(node, indent);
        break;

    case HIR_RETURN:
        fprintf(stderr, "\n");
        if (node->return_stmt.value != NULL) {
            hir_dump(node->return_stmt.value, indent + 1);
        }
        break;

    case HIR_ASSIGN:
        fprintf(stderr, "\n");
        hir_dump(node->assign.target, indent + 1);
        hir_dump(node->assign.value, indent + 1);
        break;

    case HIR_BREAK:
        fprintf(stderr, "\n");
        if (node->break_stmt.value != NULL) {
            hir_dump(node->break_stmt.value, indent + 1);
        }
        break;

    case HIR_CONTINUE:
        fprintf(stderr, "\n");
        break;

    case HIR_DEFER:
        fprintf(stderr, "\n");
        hir_dump(node->defer_stmt.body, indent + 1);
        break;

    case HIR_BOOL_LIT:
    case HIR_INT_LIT:
    case HIR_FLOAT_LIT:
    case HIR_CHAR_LIT:
    case HIR_STR_LIT:
    case HIR_UNIT_LIT:
        dump_lit(node);
        break;

    case HIR_ARRAY_LIT:
        fprintf(stderr, "\n");
        hir_dump_children(node->array_lit.elems, BUF_LEN(node->array_lit.elems), indent + 1);
        break;

    case HIR_SLICE_LIT:
        fprintf(stderr, "\n");
        hir_dump_children(node->slice_lit.elems, BUF_LEN(node->slice_lit.elems), indent + 1);
        break;

    case HIR_TUPLE_LIT:
        fprintf(stderr, "\n");
        hir_dump_children(node->tuple_lit.elems, BUF_LEN(node->tuple_lit.elems), indent + 1);
        break;

    case HIR_VAR_REF:
        fprintf(stderr, " \"%s\"\n", hir_sym_name(node->var_ref.sym));
        break;

    case HIR_MODULE_ACCESS:
        fprintf(stderr, " .%s\n", node->module_access.member);
        hir_dump(node->module_access.object, indent + 1);
        break;

    case HIR_IDX:
        fprintf(stderr, "\n");
        hir_dump(node->idx_access.object, indent + 1);
        hir_dump(node->idx_access.idx, indent + 1);
        break;

    case HIR_SLICE_EXPR:
        fprintf(stderr, "%s\n", node->slice_expr.from_array ? " (from_array)" : "");
        hir_dump(node->slice_expr.object, indent + 1);
        if (node->slice_expr.start != NULL) {
            hir_dump(node->slice_expr.start, indent + 1);
        }
        if (node->slice_expr.end != NULL) {
            hir_dump(node->slice_expr.end, indent + 1);
        }
        break;

    case HIR_TUPLE_IDX:
        fprintf(stderr, " .%d\n", node->tuple_idx.elem_idx);
        hir_dump(node->tuple_idx.object, indent + 1);
        break;

    case HIR_UNARY:
    case HIR_BINARY:
        dump_unary_binary(node, indent);
        break;

    case HIR_CALL:
        fprintf(stderr, "\n");
        hir_dump(node->call.callee, indent + 1);
        hir_dump_children(node->call.args, BUF_LEN(node->call.args), indent + 1);
        break;

    case HIR_TYPE_CONVERSION:
        fprintf(stderr, "\n");
        hir_dump(node->type_conversion.operand, indent + 1);
        break;

    case HIR_IF:
        dump_if_node(node, indent);
        break;

    case HIR_BLOCK:
        dump_block(node, indent);
        break;

    case HIR_LOOP:
        fprintf(stderr, "\n");
        hir_dump(node->loop.body, indent + 1);
        break;

    case HIR_STRUCT_DECL:
        fprintf(stderr, " \"%s\"\n", node->struct_decl.name);
        break;

    case HIR_STRUCT_LIT:
        fprintf(stderr, "\n");
        hir_dump_children(node->struct_lit.field_values, BUF_LEN(node->struct_lit.field_values),
                         indent + 1);
        break;

    case HIR_STRUCT_FIELD_ACCESS:
        fprintf(stderr, " .%s%s\n", node->struct_field_access.field,
                node->struct_field_access.via_ptr ? " (ptr)" : "");
        hir_dump(node->struct_field_access.object, indent + 1);
        break;

    case HIR_METHOD_CALL:
        fprintf(stderr, " %s\n", node->method_call.mangled_name);
        hir_dump(node->method_call.recv, indent + 1);
        hir_dump_children(node->method_call.args, BUF_LEN(node->method_call.args), indent + 1);
        break;

    case HIR_HEAP_ALLOC:
        fprintf(stderr, "\n");
        hir_dump(node->heap_alloc.operand, indent + 1);
        break;

    case HIR_ADDRESS_OF:
        fprintf(stderr, "\n");
        hir_dump(node->address_of.operand, indent + 1);
        break;

    case HIR_DEREF:
        fprintf(stderr, "\n");
        hir_dump(node->deref.operand, indent + 1);
        break;

    case HIR_ENUM_DECL:
        fprintf(stderr, " \"%s\"\n", node->enum_decl.name);
        break;

    case HIR_MATCH:
        dump_match_node(node, indent);
        break;
    }
}

static void hir_dump_children(HirNode **children, int32_t count, int32_t indent) {
    for (int32_t i = 0; i < count; i++) {
        hir_dump(children[i], indent);
    }
}

// ── TT child visitor ──────────────────────────────────────────────────

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
        visit_buf(visitor, ctx, node->match_expr.arm_conds, BUF_LEN(node->match_expr.arm_conds));
        visit_buf(visitor, ctx, node->match_expr.arm_guards, BUF_LEN(node->match_expr.arm_guards));
        visit_buf(visitor, ctx, node->match_expr.arm_bodies, BUF_LEN(node->match_expr.arm_bodies));
        break;
    default:
        break;
    }
}
