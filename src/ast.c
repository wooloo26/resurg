#include "ast.h"

// ------------------------------------------------------------------------
// Static helpers
// ------------------------------------------------------------------------
static void indent(int32_t level) {
    for (int32_t i = 0; i < level; i++) {
        fprintf(stderr, "  ");
    }
}

// ------------------------------------------------------------------------
// Per-node dump helpers
// ------------------------------------------------------------------------
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

static void dump_assert_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "Assert\n");
    ast_dump(node->assert_stmt.condition, level + 1);
    if (node->assert_stmt.message != NULL) {
        ast_dump(node->assert_stmt.message, level + 1);
    }
}

static void dump_literal_node(const ASTNode *node) {
    fprintf(stderr, "Literal(");
    switch (node->literal.kind) {
    case LIT_BOOL:
        fprintf(stderr, "%s", node->literal.boolean_value ? "true" : "false");
        break;
    case LIT_I32:
        fprintf(stderr, "%lld", (long long)node->literal.integer_value);
        break;
    case LIT_U32:
        fprintf(stderr, "%llu", (unsigned long long)(uint64_t)node->literal.integer_value);
        break;
    case LIT_F64:
        fprintf(stderr, "%g", node->literal.float64_value);
        break;
    case LIT_STR:
        fprintf(stderr, "\"%s\"", node->literal.string_value);
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
    ast_dump(node->if_expr.condition, level + 1);
    ast_dump(node->if_expr.then_body, level + 1);
    if (node->if_expr.else_body != NULL) {
        ast_dump(node->if_expr.else_body, level + 1);
    }
}

static void dump_for_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "For(%s)\n", node->for_loop.var_name);
    ast_dump(node->for_loop.start, level + 1);
    ast_dump(node->for_loop.end, level + 1);
    ast_dump(node->for_loop.body, level + 1);
}

static void dump_block_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "Block\n");
    for (int32_t i = 0; i < BUF_LEN(node->block.stmts); i++) {
        ast_dump(node->block.stmts[i], level + 1);
    }
    if (node->block.result != NULL) {
        indent(level + 1);
        fprintf(stderr, "=> ");
        ast_dump(node->block.result, 0);
    }
}

static void dump_str_interp_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "StrInterp\n");
    for (int32_t i = 0; i < BUF_LEN(node->str_interp.parts); i++) {
        ast_dump(node->str_interp.parts[i], level + 1);
    }
}

// ------------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------------
ASTNode *ast_new(Arena *a, NodeKind kind, SrcLoc loc) {
    ASTNode *n = arena_alloc(a, sizeof(ASTNode));
    memset(n, 0, sizeof(ASTNode));
    n->kind = kind;
    n->loc = loc;
    n->type = NULL;
    return n;
}

void ast_dump(const ASTNode *node, int32_t level) {
    if (node == NULL) {
        indent(level);
        fprintf(stderr, "(null)\n");
        return;
    }
    indent(level);
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
        fprintf(stderr, "VarDecl(%s, %s)\n", node->var_decl.name, node->var_decl.is_var ? "var" : ":=");
        ast_dump(node->var_decl.initializer, level + 1);
        break;
    case NODE_EXPR_STMT:
        fprintf(stderr, "ExprStmt\n");
        ast_dump(node->expr_stmt.expr, level + 1);
        break;
    case NODE_ASSERT:
        dump_assert_node(node, level);
        break;
    case NODE_BREAK:
        fprintf(stderr, "Break\n");
        break;
    case NODE_CONTINUE:
        fprintf(stderr, "Continue\n");
        break;
    case NODE_LITERAL:
        dump_literal_node(node);
        break;
    case NODE_IDENT:
        fprintf(stderr, "Ident(%s)\n", node->ident.name);
        break;
    case NODE_UNARY:
        fprintf(stderr, "Unary(%s)\n", token_kind_str(node->unary.op));
        ast_dump(node->unary.operand, level + 1);
        break;
    case NODE_BINARY:
        fprintf(stderr, "Binary(%s)\n", token_kind_str(node->binary.op));
        ast_dump(node->binary.left, level + 1);
        ast_dump(node->binary.right, level + 1);
        break;
    case NODE_ASSIGN:
        fprintf(stderr, "Assign\n");
        ast_dump(node->assign.target, level + 1);
        ast_dump(node->assign.value, level + 1);
        break;
    case NODE_COMPOUND_ASSIGN:
        fprintf(stderr, "CompoundAssign(%s)\n", token_kind_str(node->compound_assign.op));
        ast_dump(node->compound_assign.target, level + 1);
        ast_dump(node->compound_assign.value, level + 1);
        break;
    case NODE_CALL:
        dump_call_node(node, level);
        break;
    case NODE_MEMBER:
        fprintf(stderr, "Member(.%s)\n", node->member.member);
        ast_dump(node->member.object, level + 1);
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
    case NODE_STR_INTERP:
        dump_str_interp_node(node, level);
        break;
    }
}
