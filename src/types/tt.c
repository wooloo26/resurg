#include "types/tt.h"

#include "sema/_sema.h"

// ── TTSym ───────────────────────────────────────────────────────────

const char *tt_sym_name(const TTSym *sym) {
    assert(sym != NULL && sym->sema_sym != NULL);
    return sym->sema_sym->name;
}

const Type *tt_sym_type(const TTSym *sym) {
    assert(sym != NULL && sym->sema_sym != NULL);
    return sym->sema_sym->type;
}

TTSym *tt_sym_new(Arena *arena, TtSymKind kind, Sym *sema_sym, bool is_mut, SourceLoc loc) {
    TTSym *sym = arena_alloc_zero(arena, sizeof(TTSym));
    sym->kind = kind;
    sym->sema_sym = sema_sym;
    sym->is_mut = is_mut;
    sym->loc = loc;
    return sym;
}

// ── TTNode constructors ───────────────────────────────────────────────

TTNode *tt_new(Arena *arena, TtNodeKind kind, const Type *type, SourceLoc loc) {
    TTNode *node = arena_alloc_zero(arena, sizeof(TTNode));
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

/** Return a short str label for a TtNodeKind. */
static const char *tt_node_kind_str(TtNodeKind kind) {
    switch (kind) {
    case TT_FILE:
        return "File";
    case TT_MODULE:
        return "Module";
    case TT_TYPE_ALIAS:
        return "TypeAlias";
    case TT_FN_DECL:
        return "FnDecl";
    case TT_PARAM:
        return "Param";
    case TT_VAR_DECL:
        return "VarDecl";
    case TT_RETURN:
        return "Return";
    case TT_ASSIGN:
        return "Assign";
    case TT_BREAK:
        return "Break";
    case TT_CONTINUE:
        return "Continue";
    case TT_DEFER:
        return "Defer";
    case TT_BOOL_LIT:
        return "BoolLit";
    case TT_INT_LIT:
        return "IntLit";
    case TT_FLOAT_LIT:
        return "FloatLit";
    case TT_CHAR_LIT:
        return "CharLit";
    case TT_STR_LIT:
        return "StrLit";
    case TT_UNIT_LIT:
        return "UnitLit";
    case TT_ARRAY_LIT:
        return "ArrayLit";
    case TT_SLICE_LIT:
        return "SliceLit";
    case TT_TUPLE_LIT:
        return "TupleLit";
    case TT_VAR_REF:
        return "VarRef";
    case TT_MODULE_ACCESS:
        return "ModuleAccess";
    case TT_IDX:
        return "Idx";
    case TT_SLICE_EXPR:
        return "SliceExpr";
    case TT_TUPLE_IDX:
        return "TupleIdx";
    case TT_UNARY:
        return "Unary";
    case TT_BINARY:
        return "Binary";
    case TT_CALL:
        return "Call";
    case TT_TYPE_CONVERSION:
        return "TypeConv";
    case TT_IF:
        return "If";
    case TT_BLOCK:
        return "Block";
    case TT_LOOP:
        return "Loop";
    case TT_STRUCT_DECL:
        return "StructDecl";
    case TT_STRUCT_LIT:
        return "StructLit";
    case TT_STRUCT_FIELD_ACCESS:
        return "StructFieldAccess";
    case TT_METHOD_CALL:
        return "MethodCall";
    case TT_HEAP_ALLOC:
        return "HeapAlloc";
    case TT_ADDRESS_OF:
        return "AddressOf";
    case TT_DEREF:
        return "Deref";
    case TT_ENUM_DECL:
        return "EnumDecl";
    case TT_MATCH:
        return "Match";
    }
    return "?";
}

static void tt_dump_children(TTNode **children, int32_t count, int32_t indent);

static void dump_lit(const TTNode *node) {
    switch (node->kind) {
    case TT_BOOL_LIT:
        fprintf(stderr, " %s\n", node->bool_lit.value ? "true" : "false");
        break;
    case TT_INT_LIT:
        fprintf(stderr, " %lu\n", (unsigned long)node->int_lit.value);
        break;
    case TT_FLOAT_LIT:
        fprintf(stderr, " %g\n", node->float_lit.value);
        break;
    case TT_CHAR_LIT:
        fprintf(stderr, " U+%04X\n", node->char_lit.value);
        break;
    case TT_STR_LIT:
        fprintf(stderr, " \"%s\"\n", node->str_lit.value);
        break;
    default:
        fprintf(stderr, "\n");
        break;
    }
}

static void dump_unary_binary(const TTNode *node, int32_t indent) {
    if (node->kind == TT_UNARY) {
        fprintf(stderr, " %s\n", token_kind_str(node->unary.op));
        tt_dump(node->unary.operand, indent + 1);
    } else {
        fprintf(stderr, " %s\n", token_kind_str(node->binary.op));
        tt_dump(node->binary.left, indent + 1);
        tt_dump(node->binary.right, indent + 1);
    }
}

static void dump_fn_decl(const TTNode *node, int32_t indent) {
    fprintf(stderr, " \"%s\"%s\n", node->fn_decl.name, node->fn_decl.is_pub ? " pub" : "");
    tt_dump_children(node->fn_decl.params, BUF_LEN(node->fn_decl.params), indent + 1);
    if (node->fn_decl.body != NULL) {
        tt_dump(node->fn_decl.body, indent + 1);
    }
}

static void dump_var_decl(const TTNode *node, int32_t indent) {
    fprintf(stderr, " \"%s\"%s\n", node->var_decl.name, node->var_decl.is_mut ? " mut" : "");
    if (node->var_decl.init != NULL) {
        tt_dump(node->var_decl.init, indent + 1);
    }
}

static void dump_block(const TTNode *node, int32_t indent) {
    fprintf(stderr, "\n");
    tt_dump_children(node->block.stmts, BUF_LEN(node->block.stmts), indent + 1);
    if (node->block.result != NULL) {
        dump_indent(indent + 1);
        fprintf(stderr, "result:\n");
        tt_dump(node->block.result, indent + 2);
    }
}

static void dump_match_node(const TTNode *node, int32_t indent) {
    fprintf(stderr, "\n");
    tt_dump(node->match_expr.operand, indent + 1);
    for (int32_t i = 0; i < BUF_LEN(node->match_expr.arm_bodies); i++) {
        dump_indent(indent + 1);
        fprintf(stderr, "arm %d:\n", i);
        if (node->match_expr.arm_conds[i] != NULL) {
            tt_dump(node->match_expr.arm_conds[i], indent + 2);
        }
        if (node->match_expr.arm_guards[i] != NULL) {
            dump_indent(indent + 2);
            fprintf(stderr, "guard:\n");
            tt_dump(node->match_expr.arm_guards[i], indent + 3);
        }
        tt_dump(node->match_expr.arm_bodies[i], indent + 2);
    }
}

static void dump_if_node(const TTNode *node, int32_t indent) {
    fprintf(stderr, "\n");
    tt_dump(node->if_expr.cond, indent + 1);
    tt_dump(node->if_expr.then_body, indent + 1);
    if (node->if_expr.else_body != NULL) {
        tt_dump(node->if_expr.else_body, indent + 1);
    }
}

void tt_dump(const TTNode *node, int32_t indent) {
    if (node == NULL) {
        dump_indent(indent);
        fprintf(stderr, "<null>\n");
        return;
    }

    dump_indent(indent);
    fprintf(stderr, "%s", tt_node_kind_str(node->kind));

    switch (node->kind) {
    case TT_FILE:
        fprintf(stderr, "\n");
        tt_dump_children(node->file.decls, BUF_LEN(node->file.decls), indent + 1);
        break;

    case TT_MODULE:
        fprintf(stderr, " \"%s\"\n", node->module.name);
        break;

    case TT_TYPE_ALIAS:
        fprintf(stderr, " \"%s\"%s\n", node->type_alias.name,
                node->type_alias.is_pub ? " pub" : "");
        break;

    case TT_FN_DECL:
        dump_fn_decl(node, indent);
        break;

    case TT_PARAM:
        fprintf(stderr, " \"%s\"\n", node->param.name);
        break;

    case TT_VAR_DECL:
        dump_var_decl(node, indent);
        break;

    case TT_RETURN:
        fprintf(stderr, "\n");
        if (node->return_stmt.value != NULL) {
            tt_dump(node->return_stmt.value, indent + 1);
        }
        break;

    case TT_ASSIGN:
        fprintf(stderr, "\n");
        tt_dump(node->assign.target, indent + 1);
        tt_dump(node->assign.value, indent + 1);
        break;

    case TT_BREAK:
        fprintf(stderr, "\n");
        if (node->break_stmt.value != NULL) {
            tt_dump(node->break_stmt.value, indent + 1);
        }
        break;

    case TT_CONTINUE:
        fprintf(stderr, "\n");
        break;

    case TT_DEFER:
        fprintf(stderr, "\n");
        tt_dump(node->defer_stmt.body, indent + 1);
        break;

    case TT_BOOL_LIT:
    case TT_INT_LIT:
    case TT_FLOAT_LIT:
    case TT_CHAR_LIT:
    case TT_STR_LIT:
    case TT_UNIT_LIT:
        dump_lit(node);
        break;

    case TT_ARRAY_LIT:
        fprintf(stderr, "\n");
        tt_dump_children(node->array_lit.elems, BUF_LEN(node->array_lit.elems), indent + 1);
        break;

    case TT_SLICE_LIT:
        fprintf(stderr, "\n");
        tt_dump_children(node->slice_lit.elems, BUF_LEN(node->slice_lit.elems), indent + 1);
        break;

    case TT_TUPLE_LIT:
        fprintf(stderr, "\n");
        tt_dump_children(node->tuple_lit.elems, BUF_LEN(node->tuple_lit.elems), indent + 1);
        break;

    case TT_VAR_REF:
        fprintf(stderr, " \"%s\"\n", tt_sym_name(node->var_ref.sym));
        break;

    case TT_MODULE_ACCESS:
        fprintf(stderr, " .%s\n", node->module_access.member);
        tt_dump(node->module_access.object, indent + 1);
        break;

    case TT_IDX:
        fprintf(stderr, "\n");
        tt_dump(node->idx_access.object, indent + 1);
        tt_dump(node->idx_access.idx, indent + 1);
        break;

    case TT_SLICE_EXPR:
        fprintf(stderr, "%s\n", node->slice_expr.from_array ? " (from_array)" : "");
        tt_dump(node->slice_expr.object, indent + 1);
        if (node->slice_expr.start != NULL) {
            tt_dump(node->slice_expr.start, indent + 1);
        }
        if (node->slice_expr.end != NULL) {
            tt_dump(node->slice_expr.end, indent + 1);
        }
        break;

    case TT_TUPLE_IDX:
        fprintf(stderr, " .%d\n", node->tuple_idx.elem_idx);
        tt_dump(node->tuple_idx.object, indent + 1);
        break;

    case TT_UNARY:
    case TT_BINARY:
        dump_unary_binary(node, indent);
        break;

    case TT_CALL:
        fprintf(stderr, "\n");
        tt_dump(node->call.callee, indent + 1);
        tt_dump_children(node->call.args, BUF_LEN(node->call.args), indent + 1);
        break;

    case TT_TYPE_CONVERSION:
        fprintf(stderr, "\n");
        tt_dump(node->type_conversion.operand, indent + 1);
        break;

    case TT_IF:
        dump_if_node(node, indent);
        break;

    case TT_BLOCK:
        dump_block(node, indent);
        break;

    case TT_LOOP:
        fprintf(stderr, "\n");
        tt_dump(node->loop.body, indent + 1);
        break;

    case TT_STRUCT_DECL:
        fprintf(stderr, " \"%s\"\n", node->struct_decl.name);
        break;

    case TT_STRUCT_LIT:
        fprintf(stderr, "\n");
        tt_dump_children(node->struct_lit.field_values, BUF_LEN(node->struct_lit.field_values),
                         indent + 1);
        break;

    case TT_STRUCT_FIELD_ACCESS:
        fprintf(stderr, " .%s%s\n", node->struct_field_access.field,
                node->struct_field_access.via_ptr ? " (ptr)" : "");
        tt_dump(node->struct_field_access.object, indent + 1);
        break;

    case TT_METHOD_CALL:
        fprintf(stderr, " %s\n", node->method_call.mangled_name);
        tt_dump(node->method_call.recv, indent + 1);
        tt_dump_children(node->method_call.args, BUF_LEN(node->method_call.args), indent + 1);
        break;

    case TT_HEAP_ALLOC:
        fprintf(stderr, "\n");
        tt_dump(node->heap_alloc.operand, indent + 1);
        break;

    case TT_ADDRESS_OF:
        fprintf(stderr, "\n");
        tt_dump(node->address_of.operand, indent + 1);
        break;

    case TT_DEREF:
        fprintf(stderr, "\n");
        tt_dump(node->deref.operand, indent + 1);
        break;

    case TT_ENUM_DECL:
        fprintf(stderr, " \"%s\"\n", node->enum_decl.name);
        break;

    case TT_MATCH:
        dump_match_node(node, indent);
        break;
    }
}

static void tt_dump_children(TTNode **children, int32_t count, int32_t indent) {
    for (int32_t i = 0; i < count; i++) {
        tt_dump(children[i], indent);
    }
}

// ── TT child visitor ──────────────────────────────────────────────────

static void visit_buf(TtChildVisitor visitor, void *ctx, TTNode **buf, int32_t count) {
    for (int32_t i = 0; i < count; i++) {
        visitor(ctx, &buf[i]);
    }
}

void tt_visit_children(TTNode *node, TtChildVisitor visitor, void *ctx) {
    if (node == NULL) {
        return;
    }
    switch (node->kind) {
    case TT_FILE:
        visit_buf(visitor, ctx, node->file.decls, BUF_LEN(node->file.decls));
        break;
    case TT_FN_DECL:
        visit_buf(visitor, ctx, node->fn_decl.params, BUF_LEN(node->fn_decl.params));
        if (node->fn_decl.body != NULL) {
            visitor(ctx, &node->fn_decl.body);
        }
        break;
    case TT_BLOCK:
        visit_buf(visitor, ctx, node->block.stmts, BUF_LEN(node->block.stmts));
        if (node->block.result != NULL) {
            visitor(ctx, &node->block.result);
        }
        break;
    case TT_VAR_DECL:
        if (node->var_decl.init != NULL) {
            visitor(ctx, &node->var_decl.init);
        }
        break;
    case TT_RETURN:
        if (node->return_stmt.value != NULL) {
            visitor(ctx, &node->return_stmt.value);
        }
        break;
    case TT_ASSIGN:
        visitor(ctx, &node->assign.target);
        visitor(ctx, &node->assign.value);
        break;
    case TT_BINARY:
        visitor(ctx, &node->binary.left);
        visitor(ctx, &node->binary.right);
        break;
    case TT_UNARY:
        visitor(ctx, &node->unary.operand);
        break;
    case TT_CALL:
        visitor(ctx, &node->call.callee);
        visit_buf(visitor, ctx, node->call.args, BUF_LEN(node->call.args));
        break;
    case TT_IF:
        visitor(ctx, &node->if_expr.cond);
        visitor(ctx, &node->if_expr.then_body);
        if (node->if_expr.else_body != NULL) {
            visitor(ctx, &node->if_expr.else_body);
        }
        break;
    case TT_ARRAY_LIT:
        visit_buf(visitor, ctx, node->array_lit.elems, BUF_LEN(node->array_lit.elems));
        break;
    case TT_SLICE_LIT:
        visit_buf(visitor, ctx, node->slice_lit.elems, BUF_LEN(node->slice_lit.elems));
        break;
    case TT_TUPLE_LIT:
        visit_buf(visitor, ctx, node->tuple_lit.elems, BUF_LEN(node->tuple_lit.elems));
        break;
    case TT_IDX:
        visitor(ctx, &node->idx_access.object);
        visitor(ctx, &node->idx_access.idx);
        break;
    case TT_SLICE_EXPR:
        visitor(ctx, &node->slice_expr.object);
        if (node->slice_expr.start != NULL) {
            visitor(ctx, &node->slice_expr.start);
        }
        if (node->slice_expr.end != NULL) {
            visitor(ctx, &node->slice_expr.end);
        }
        break;
    case TT_TUPLE_IDX:
        visitor(ctx, &node->tuple_idx.object);
        break;
    case TT_TYPE_CONVERSION:
        visitor(ctx, &node->type_conversion.operand);
        break;
    case TT_MODULE_ACCESS:
        visitor(ctx, &node->module_access.object);
        break;
    case TT_LOOP:
        visitor(ctx, &node->loop.body);
        break;
    case TT_DEFER:
        visitor(ctx, &node->defer_stmt.body);
        break;
    case TT_STRUCT_DECL:
        break;
    case TT_STRUCT_LIT:
        visit_buf(visitor, ctx, node->struct_lit.field_values,
                  BUF_LEN(node->struct_lit.field_values));
        break;
    case TT_STRUCT_FIELD_ACCESS:
        visitor(ctx, &node->struct_field_access.object);
        break;
    case TT_METHOD_CALL:
        visitor(ctx, &node->method_call.recv);
        visit_buf(visitor, ctx, node->method_call.args, BUF_LEN(node->method_call.args));
        break;
    case TT_HEAP_ALLOC:
        visitor(ctx, &node->heap_alloc.operand);
        break;
    case TT_ADDRESS_OF:
        visitor(ctx, &node->address_of.operand);
        break;
    case TT_DEREF:
        visitor(ctx, &node->deref.operand);
        break;
    case TT_ENUM_DECL:
        break;
    case TT_MATCH:
        visitor(ctx, &node->match_expr.operand);
        visit_buf(visitor, ctx, node->match_expr.arm_conds, BUF_LEN(node->match_expr.arm_conds));
        visit_buf(visitor, ctx, node->match_expr.arm_guards, BUF_LEN(node->match_expr.arm_guards));
        visit_buf(visitor, ctx, node->match_expr.arm_bodies, BUF_LEN(node->match_expr.arm_bodies));
        break;
    default:
        break;
    }
}
