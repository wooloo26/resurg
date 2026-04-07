#include "repr/ast.h"

// Indentation helper for ast_dump.
static void print_indent(int32_t level) {
    for (int32_t i = 0; i < level; i++) {
        fprintf(stderr, "  ");
    }
}

// Per-node dump helpers - each prints one node kind and recurses.
static void dump_file_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "File\n");
    for (int32_t i = 0; i < BUF_LEN(node->file.decls); i++) {
        ast_dump(node->file.decls[i], level + 1);
    }
}

static void dump_fn_decl_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "FnDecl(%s%s)\n", node->fn_decl.is_pub ? "pub " : "", node->fn_decl.name);
    for (int32_t i = 0; i < BUF_LEN(node->fn_decl.params); i++) {
        ast_dump(node->fn_decl.params[i], level + 1);
    }
    ast_dump(node->fn_decl.body, level + 1);
}

static void dump_lit_node(const ASTNode *node) {
    fprintf(stderr, "Lit(");
    switch (node->lit.kind) {
    case LIT_BOOL:
        fprintf(stderr, "%s", node->lit.boolean_value ? "true" : "false");
        break;
    case LIT_I8:
    case LIT_I16:
    case LIT_I32:
    case LIT_I64:
    case LIT_I128:
    case LIT_ISIZE:
        fprintf(stderr, "%lld", (long long)(int64_t)node->lit.integer_value);
        break;
    case LIT_U8:
    case LIT_U16:
    case LIT_U32:
    case LIT_U64:
    case LIT_U128:
    case LIT_USIZE:
        fprintf(stderr, "%llu", (unsigned long long)node->lit.integer_value);
        break;
    case LIT_F32:
    case LIT_F64:
        fprintf(stderr, "%g", node->lit.float64_value);
        break;
    case LIT_CHAR:
        if (node->lit.char_value < 128) {
            fprintf(stderr, "'%c'", (char)node->lit.char_value);
        } else {
            fprintf(stderr, "'\\u{%04X}'", node->lit.char_value);
        }
        break;
    case LIT_STR:
        fprintf(stderr, "\"%s\"", node->lit.str_value);
        break;
    case LIT_UNIT:
        fprintf(stderr, "unit");
        break;
    }
    fprintf(stderr, ")\n");
}

static void dump_call_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "Call\n");
    ast_dump(node->call.callee, level + 1);
    for (int32_t i = 0; i < BUF_LEN(node->call.args); i++) {
        ast_dump(node->call.args[i], level + 1);
    }
}

static void dump_if_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "If\n");
    ast_dump(node->if_expr.cond, level + 1);
    ast_dump(node->if_expr.then_body, level + 1);
    if (node->if_expr.else_body != NULL) {
        ast_dump(node->if_expr.else_body, level + 1);
    }
}

static void dump_for_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "For(%s", node->for_loop.var_name);
    if (node->for_loop.idx_name != NULL) {
        fprintf(stderr, ", %s", node->for_loop.idx_name);
    }
    fprintf(stderr, ")\n");
    if (node->for_loop.iterable != NULL) {
        ast_dump(node->for_loop.iterable, level + 1);
    } else {
        ast_dump(node->for_loop.start, level + 1);
        ast_dump(node->for_loop.end, level + 1);
    }
    ast_dump(node->for_loop.body, level + 1);
}

static void dump_block_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "Block\n");
    for (int32_t i = 0; i < BUF_LEN(node->block.stmts); i++) {
        ast_dump(node->block.stmts[i], level + 1);
    }
    if (node->block.result != NULL) {
        print_indent(level + 1);
        fprintf(stderr, "=> ");
        ast_dump(node->block.result, 0);
    }
}

static void dump_str_interpolation_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "StrInterp\n");
    for (int32_t i = 0; i < BUF_LEN(node->str_interpolation.parts); i++) {
        ast_dump(node->str_interpolation.parts[i], level + 1);
    }
}

ASTNode *ast_new(Arena *arena, NodeKind kind, SrcLoc loc) {
    ASTNode *node = arena_alloc_zero(arena, sizeof(ASTNode));
    node->kind = kind;
    node->loc = loc;
    node->type = NULL;
    return node;
}

static void dump_unary_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "Unary(%s)\n", token_kind_str(node->unary.op));
    ast_dump(node->unary.operand, level + 1);
}

static void dump_binary_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "Binary(%s)\n", token_kind_str(node->binary.op));
    ast_dump(node->binary.left, level + 1);
    ast_dump(node->binary.right, level + 1);
}

static void dump_assign_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "Assign\n");
    ast_dump(node->assign.target, level + 1);
    ast_dump(node->assign.value, level + 1);
}

static void dump_compound_assign_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "CompoundAssign(%s)\n", token_kind_str(node->compound_assign.op));
    ast_dump(node->compound_assign.target, level + 1);
    ast_dump(node->compound_assign.value, level + 1);
}

static void dump_array_lit_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "ArrayLit(%d)\n", node->array_lit.size);
    for (int32_t i = 0; i < BUF_LEN(node->array_lit.elems); i++) {
        ast_dump(node->array_lit.elems[i], level + 1);
    }
}

static void dump_tuple_lit_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "TupleLit\n");
    for (int32_t i = 0; i < BUF_LEN(node->tuple_lit.elems); i++) {
        ast_dump(node->tuple_lit.elems[i], level + 1);
    }
}

static void dump_struct_decl_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "StructDecl(%s)\n", node->struct_decl.name);
    for (int32_t i = 0; i < BUF_LEN(node->struct_decl.methods); i++) {
        ast_dump(node->struct_decl.methods[i], level + 1);
    }
}

static void dump_struct_lit_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "StructLit(%s)\n", node->struct_lit.name);
    for (int32_t i = 0; i < BUF_LEN(node->struct_lit.field_values); i++) {
        ast_dump(node->struct_lit.field_values[i], level + 1);
    }
}

static void dump_enum_decl_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "EnumDecl(%s)\n", node->enum_decl.name);
    for (int32_t i = 0; i < BUF_LEN(node->enum_decl.methods); i++) {
        ast_dump(node->enum_decl.methods[i], level + 1);
    }
}

static void dump_match_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "Match\n");
    ast_dump(node->match_expr.operand, level + 1);
    for (int32_t i = 0; i < BUF_LEN(node->match_expr.arms); i++) {
        print_indent(level + 1);
        fprintf(stderr, "Arm\n");
        ast_dump(node->match_expr.arms[i].body, level + 2);
    }
}

static void dump_enum_init_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "EnumInit(%s::%s)\n", node->enum_init.enum_name, node->enum_init.variant_name);
    for (int32_t i = 0; i < BUF_LEN(node->enum_init.args); i++) {
        ast_dump(node->enum_init.args[i], level + 1);
    }
}

void ast_dump(const ASTNode *node, int32_t level) {
    if (node == NULL) {
        print_indent(level);
        fprintf(stderr, "(null)\n");
        return;
    }
    print_indent(level);
    switch (node->kind) {
    case NODE_MODULE:
        fprintf(stderr, "Module(%s)\n", node->module.name);
        break;
    case NODE_FILE:
        dump_file_node(node, level);
        break;
    case NODE_FN_DECL:
        dump_fn_decl_node(node, level);
        break;
    case NODE_PARAM:
        fprintf(stderr, "Param(%s)\n", node->param.name);
        break;
    case NODE_VAR_DECL:
        fprintf(stderr, "VarDecl(%s, %s)\n", node->var_decl.name,
                node->var_decl.is_var ? "var" : ":=");
        ast_dump(node->var_decl.init, level + 1);
        break;
    case NODE_EXPR_STMT:
        fprintf(stderr, "ExprStmt\n");
        ast_dump(node->expr_stmt.expr, level + 1);
        break;
    case NODE_BREAK:
        fprintf(stderr, "Break\n");
        break;
    case NODE_CONTINUE:
        fprintf(stderr, "Continue\n");
        break;
    case NODE_LIT:
        dump_lit_node(node);
        break;
    case NODE_ID:
        fprintf(stderr, "Ident(%s)\n", node->id.name);
        break;
    case NODE_UNARY:
        dump_unary_node(node, level);
        break;
    case NODE_BINARY:
        dump_binary_node(node, level);
        break;
    case NODE_ASSIGN:
        dump_assign_node(node, level);
        break;
    case NODE_COMPOUND_ASSIGN:
        dump_compound_assign_node(node, level);
        break;
    case NODE_CALL:
        dump_call_node(node, level);
        break;
    case NODE_MEMBER:
        fprintf(stderr, "Member(.%s)\n", node->member.member);
        ast_dump(node->member.object, level + 1);
        break;
    case NODE_IDX:
        fprintf(stderr, "Idx\n");
        ast_dump(node->idx_access.object, level + 1);
        ast_dump(node->idx_access.idx, level + 1);
        break;
    case NODE_IF:
        dump_if_node(node, level);
        break;
    case NODE_LOOP:
        fprintf(stderr, "Loop\n");
        ast_dump(node->loop.body, level + 1);
        break;
    case NODE_FOR:
        dump_for_node(node, level);
        break;
    case NODE_BLOCK:
        dump_block_node(node, level);
        break;
    case NODE_STR_INTERPOLATION:
        dump_str_interpolation_node(node, level);
        break;
    case NODE_ARRAY_LIT:
        dump_array_lit_node(node, level);
        break;
    case NODE_TUPLE_LIT:
        dump_tuple_lit_node(node, level);
        break;
    case NODE_TYPE_CONVERSION:
        fprintf(stderr, "TypeConversion\n");
        ast_dump(node->type_conversion.operand, level + 1);
        break;
    case NODE_TYPE_ALIAS:
        fprintf(stderr, "TypeAlias(%s)\n", node->type_alias.name);
        break;
    case NODE_STRUCT_DECL:
        dump_struct_decl_node(node, level);
        break;
    case NODE_STRUCT_LIT:
        dump_struct_lit_node(node, level);
        break;
    case NODE_STRUCT_DESTRUCTURE:
        fprintf(stderr, "StructDestructure\n");
        ast_dump(node->struct_destructure.value, level + 1);
        break;
    case NODE_TUPLE_DESTRUCTURE:
        fprintf(stderr, "TupleDestructure%s\n", node->tuple_destructure.has_rest ? " [..]" : "");
        ast_dump(node->tuple_destructure.value, level + 1);
        break;
    case NODE_ADDRESS_OF:
        fprintf(stderr, "AddressOf\n");
        ast_dump(node->address_of.operand, level + 1);
        break;
    case NODE_DEREF:
        fprintf(stderr, "Deref\n");
        ast_dump(node->deref.operand, level + 1);
        break;
    case NODE_ENUM_DECL:
        dump_enum_decl_node(node, level);
        break;
    case NODE_MATCH:
        dump_match_node(node, level);
        break;
    case NODE_ENUM_INIT:
        dump_enum_init_node(node, level);
        break;
    case NODE_RETURN:
        fprintf(stderr, "Return\n");
        if (node->return_stmt.value != NULL) {
            ast_dump(node->return_stmt.value, level + 1);
        }
        break;
    case NODE_PACT_DECL:
        fprintf(stderr, "PactDecl(%s)\n", node->pact_decl.name);
        for (int32_t i = 0; i < BUF_LEN(node->pact_decl.methods); i++) {
            ast_dump(node->pact_decl.methods[i], level + 1);
        }
        break;
    case NODE_WHILE:
        fprintf(stderr, "While\n");
        ast_dump(node->while_loop.cond, level + 1);
        ast_dump(node->while_loop.body, level + 1);
        break;
    case NODE_DEFER:
        fprintf(stderr, "Defer\n");
        ast_dump(node->defer_stmt.body, level + 1);
        break;
    case NODE_SLICE_LIT:
        fprintf(stderr, "SliceLit(%d)\n", BUF_LEN(node->slice_lit.elems));
        for (int32_t i = 0; i < BUF_LEN(node->slice_lit.elems); i++) {
            ast_dump(node->slice_lit.elems[i], level + 1);
        }
        break;
    case NODE_SLICE_EXPR:
        fprintf(stderr, "SliceExpr%s\n", node->slice_expr.full_range ? "[..]" : "");
        ast_dump(node->slice_expr.object, level + 1);
        if (node->slice_expr.start != NULL) {
            ast_dump(node->slice_expr.start, level + 1);
        }
        if (node->slice_expr.end != NULL) {
            ast_dump(node->slice_expr.end, level + 1);
        }
        break;
    case NODE_OPTIONAL_CHAIN:
        fprintf(stderr, "OptionalChain(.%s)\n", node->optional_chain.member);
        ast_dump(node->optional_chain.object, level + 1);
        break;
    case NODE_TRY:
        fprintf(stderr, "Try\n");
        ast_dump(node->try_expr.operand, level + 1);
        break;
    case NODE_CLOSURE:
        fprintf(stderr, "Closure(%d params)\n", BUF_LEN(node->closure.params));
        for (int32_t i = 0; i < BUF_LEN(node->closure.params); i++) {
            ast_dump(node->closure.params[i], level + 1);
        }
        ast_dump(node->closure.body, level + 1);
        break;
    }
}

// ── Deep clone ────────────────────────────────────────────────────

ASTNode *ast_clone(Arena *arena, ASTNode *src) {
    if (src == NULL) {
        return NULL;
    }
    ASTNode *dst = ast_new(arena, src->kind, src->loc);
    switch (src->kind) {
    case NODE_LIT:
        dst->lit = src->lit;
        break;
    case NODE_ID:
        dst->id = src->id;
        break;
    case NODE_UNARY:
        dst->unary.op = src->unary.op;
        dst->unary.operand = ast_clone(arena, src->unary.operand);
        break;
    case NODE_BINARY:
        dst->binary.op = src->binary.op;
        dst->binary.left = ast_clone(arena, src->binary.left);
        dst->binary.right = ast_clone(arena, src->binary.right);
        break;
    case NODE_BLOCK:
        dst->block.stmts = NULL;
        for (int32_t i = 0; i < BUF_LEN(src->block.stmts); i++) {
            BUF_PUSH(dst->block.stmts, ast_clone(arena, src->block.stmts[i]));
        }
        dst->block.result = ast_clone(arena, src->block.result);
        break;
    case NODE_EXPR_STMT:
        dst->expr_stmt.expr = ast_clone(arena, src->expr_stmt.expr);
        break;
    case NODE_VAR_DECL:
        dst->var_decl = src->var_decl;
        dst->var_decl.init = ast_clone(arena, src->var_decl.init);
        break;
    case NODE_CALL:
        dst->call.callee = ast_clone(arena, src->call.callee);
        dst->call.args = NULL;
        dst->call.arg_names = NULL;
        dst->call.arg_is_mut = NULL;
        dst->call.type_args = NULL;
        for (int32_t i = 0; i < BUF_LEN(src->call.args); i++) {
            BUF_PUSH(dst->call.args, ast_clone(arena, src->call.args[i]));
        }
        for (int32_t i = 0; i < BUF_LEN(src->call.arg_names); i++) {
            BUF_PUSH(dst->call.arg_names, src->call.arg_names[i]);
        }
        for (int32_t i = 0; i < BUF_LEN(src->call.arg_is_mut); i++) {
            BUF_PUSH(dst->call.arg_is_mut, src->call.arg_is_mut[i]);
        }
        break;
    case NODE_MEMBER:
        dst->member.object = ast_clone(arena, src->member.object);
        dst->member.member = src->member.member;
        break;
    case NODE_IDX:
        dst->idx_access.object = ast_clone(arena, src->idx_access.object);
        dst->idx_access.idx = ast_clone(arena, src->idx_access.idx);
        break;
    case NODE_IF:
        dst->if_expr.cond = ast_clone(arena, src->if_expr.cond);
        dst->if_expr.then_body = ast_clone(arena, src->if_expr.then_body);
        dst->if_expr.else_body = ast_clone(arena, src->if_expr.else_body);
        break;
    case NODE_RETURN:
        dst->return_stmt.value = ast_clone(arena, src->return_stmt.value);
        break;
    case NODE_ASSIGN:
        dst->assign.target = ast_clone(arena, src->assign.target);
        dst->assign.value = ast_clone(arena, src->assign.value);
        break;
    case NODE_COMPOUND_ASSIGN:
        dst->compound_assign.op = src->compound_assign.op;
        dst->compound_assign.target = ast_clone(arena, src->compound_assign.target);
        dst->compound_assign.value = ast_clone(arena, src->compound_assign.value);
        break;
    case NODE_STR_INTERPOLATION:
        dst->str_interpolation.parts = NULL;
        for (int32_t i = 0; i < BUF_LEN(src->str_interpolation.parts); i++) {
            BUF_PUSH(dst->str_interpolation.parts,
                     ast_clone(arena, src->str_interpolation.parts[i]));
        }
        break;
    case NODE_TUPLE_LIT:
        dst->tuple_lit.elems = NULL;
        for (int32_t i = 0; i < BUF_LEN(src->tuple_lit.elems); i++) {
            BUF_PUSH(dst->tuple_lit.elems, ast_clone(arena, src->tuple_lit.elems[i]));
        }
        break;
    case NODE_ARRAY_LIT:
        dst->array_lit = src->array_lit;
        dst->array_lit.elems = NULL;
        for (int32_t i = 0; i < BUF_LEN(src->array_lit.elems); i++) {
            BUF_PUSH(dst->array_lit.elems, ast_clone(arena, src->array_lit.elems[i]));
        }
        break;
    case NODE_STRUCT_LIT:
        dst->struct_lit.name = src->struct_lit.name;
        dst->struct_lit.field_names = NULL;
        dst->struct_lit.field_values = NULL;
        for (int32_t i = 0; i < BUF_LEN(src->struct_lit.field_names); i++) {
            BUF_PUSH(dst->struct_lit.field_names, src->struct_lit.field_names[i]);
        }
        for (int32_t i = 0; i < BUF_LEN(src->struct_lit.field_values); i++) {
            BUF_PUSH(dst->struct_lit.field_values,
                     ast_clone(arena, src->struct_lit.field_values[i]));
        }
        break;
    case NODE_ADDRESS_OF:
        dst->address_of.operand = ast_clone(arena, src->address_of.operand);
        break;
    case NODE_DEREF:
        dst->deref.operand = ast_clone(arena, src->deref.operand);
        break;
    case NODE_TYPE_CONVERSION:
        dst->type_conversion.target_type = src->type_conversion.target_type;
        dst->type_conversion.operand = ast_clone(arena, src->type_conversion.operand);
        break;
    case NODE_LOOP:
        dst->loop.body = ast_clone(arena, src->loop.body);
        break;
    case NODE_WHILE:
        dst->while_loop.cond = ast_clone(arena, src->while_loop.cond);
        dst->while_loop.body = ast_clone(arena, src->while_loop.body);
        break;
    case NODE_FOR:
        dst->for_loop = src->for_loop;
        dst->for_loop.start = ast_clone(arena, src->for_loop.start);
        dst->for_loop.end = ast_clone(arena, src->for_loop.end);
        dst->for_loop.iterable = ast_clone(arena, src->for_loop.iterable);
        dst->for_loop.body = ast_clone(arena, src->for_loop.body);
        break;
    case NODE_BREAK:
        dst->break_stmt.value = ast_clone(arena, src->break_stmt.value);
        break;
    case NODE_CONTINUE:
        break;
    case NODE_DEFER:
        dst->defer_stmt.body = ast_clone(arena, src->defer_stmt.body);
        break;
    case NODE_PARAM:
        dst->param = src->param;
        break;
    case NODE_CLOSURE:
        dst->closure.return_type = src->closure.return_type;
        dst->closure.params = NULL;
        for (int32_t i = 0; i < BUF_LEN(src->closure.params); i++) {
            BUF_PUSH(dst->closure.params, ast_clone(arena, src->closure.params[i]));
        }
        dst->closure.body = ast_clone(arena, src->closure.body);
        break;
    default:
        // Shallow copy for any unhandled kinds
        *dst = *src;
        dst->type = NULL;
        break;
    }
    return dst;
}
