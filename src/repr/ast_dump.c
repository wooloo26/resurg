#include "repr/ast.h"

// ── Indentation helper ─────────────────────────────────────────────────

static void print_indent(int32_t level) {
    for (int32_t i = 0; i < level; i++) {
        fprintf(stderr, "  ");
    }
}

// ── Per-node dump helpers ──────────────────────────────────────────────

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

// ── Public dump entry point ────────────────────────────────────────────

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
    case NODE_EXT_DECL:
        fprintf(stderr, "ExtDecl(%s)\n", node->ext_decl.target_name);
        for (int32_t i = 0; i < BUF_LEN(node->ext_decl.methods); i++) {
            ast_dump(node->ext_decl.methods[i], level + 1);
        }
        break;
    case NODE_USE_DECL:
        fprintf(stderr, "UseDecl(%s)\n", node->use_decl.module_path);
        break;
    }
}
