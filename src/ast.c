#include "ast.h"

// Indentation helper for ast_dump.
static void print_indent(int32_t level) {
    for (int32_t i = 0; i < level; i++) {
        fprintf(stderr, "  ");
    }
}

// Per-node dump helpers — each prints one node kind and recurses.
static void dump_file_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "File\n");
    for (int32_t i = 0; i < BUFFER_LENGTH(node->file.declarations); i++) {
        ast_dump(node->file.declarations[i], level + 1);
    }
}

static void dump_function_declaration_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "FnDecl(%s%s)\n", node->function_declaration.is_public ? "pub " : "",
            node->function_declaration.name);
    for (int32_t i = 0; i < BUFFER_LENGTH(node->function_declaration.parameters); i++) {
        ast_dump(node->function_declaration.parameters[i], level + 1);
    }
    ast_dump(node->function_declaration.body, level + 1);
}

static void dump_assert_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "Assert\n");
    ast_dump(node->assert_statement.condition, level + 1);
    if (node->assert_statement.message != NULL) {
        ast_dump(node->assert_statement.message, level + 1);
    }
}

static void dump_literal_node(const ASTNode *node) {
    fprintf(stderr, "Literal(");
    switch (node->literal.kind) {
    case LITERAL_BOOL:
        fprintf(stderr, "%s", node->literal.boolean_value ? "true" : "false");
        break;
    case LITERAL_I32:
        fprintf(stderr, "%lld", (long long)node->literal.integer_value);
        break;
    case LITERAL_U32:
        fprintf(stderr, "%llu", (unsigned long long)(uint64_t)node->literal.integer_value);
        break;
    case LITERAL_F64:
        fprintf(stderr, "%g", node->literal.float64_value);
        break;
    case LITERAL_STRING:
        fprintf(stderr, "\"%s\"", node->literal.string_value);
        break;
    case LITERAL_UNIT:
        fprintf(stderr, "unit");
        break;
    }
    fprintf(stderr, ")\n");
}

static void dump_call_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "Call\n");
    ast_dump(node->call.callee, level + 1);
    for (int32_t i = 0; i < BUFFER_LENGTH(node->call.arguments); i++) {
        ast_dump(node->call.arguments[i], level + 1);
    }
}

static void dump_if_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "If\n");
    ast_dump(node->if_expression.condition, level + 1);
    ast_dump(node->if_expression.then_body, level + 1);
    if (node->if_expression.else_body != NULL) {
        ast_dump(node->if_expression.else_body, level + 1);
    }
}

static void dump_for_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "For(%s)\n", node->for_loop.variable_name);
    ast_dump(node->for_loop.start, level + 1);
    ast_dump(node->for_loop.end, level + 1);
    ast_dump(node->for_loop.body, level + 1);
}

static void dump_block_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "Block\n");
    for (int32_t i = 0; i < BUFFER_LENGTH(node->block.statements); i++) {
        ast_dump(node->block.statements[i], level + 1);
    }
    if (node->block.result != NULL) {
        print_indent(level + 1);
        fprintf(stderr, "=> ");
        ast_dump(node->block.result, 0);
    }
}

static void dump_string_interpolation_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "StrInterp\n");
    for (int32_t i = 0; i < BUFFER_LENGTH(node->string_interpolation.parts); i++) {
        ast_dump(node->string_interpolation.parts[i], level + 1);
    }
}

ASTNode *ast_new(Arena *arena, NodeKind kind, SourceLocation location) {
    ASTNode *node = arena_alloc(arena, sizeof(ASTNode));
    memset(node, 0, sizeof(ASTNode));
    node->kind = kind;
    node->location = location;
    node->type = NULL;
    return node;
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
    case NODE_FUNCTION_DECLARATION:
        dump_function_declaration_node(node, level);
        break;
    case NODE_PARAMETER:
        fprintf(stderr, "Param(%s)\n", node->parameter.name);
        break;
    case NODE_VARIABLE_DECLARATION:
        fprintf(stderr, "VarDecl(%s, %s)\n", node->variable_declaration.name,
                node->variable_declaration.is_variable ? "var" : ":=");
        ast_dump(node->variable_declaration.initializer, level + 1);
        break;
    case NODE_EXPRESSION_STATEMENT:
        fprintf(stderr, "ExprStmt\n");
        ast_dump(node->expression_statement.expression, level + 1);
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
    case NODE_IDENTIFIER:
        fprintf(stderr, "Ident(%s)\n", node->identifier.name);
        break;
    case NODE_UNARY:
        fprintf(stderr, "Unary(%s)\n", token_kind_string(node->unary.operator));
        ast_dump(node->unary.operand, level + 1);
        break;
    case NODE_BINARY:
        fprintf(stderr, "Binary(%s)\n", token_kind_string(node->binary.operator));
        ast_dump(node->binary.left, level + 1);
        ast_dump(node->binary.right, level + 1);
        break;
    case NODE_ASSIGN:
        fprintf(stderr, "Assign\n");
        ast_dump(node->assign.target, level + 1);
        ast_dump(node->assign.value, level + 1);
        break;
    case NODE_COMPOUND_ASSIGN:
        fprintf(stderr, "CompoundAssign(%s)\n", token_kind_string(node->compound_assign.operator));
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
    case NODE_STRING_INTERPOLATION:
        dump_string_interpolation_node(node, level);
        break;
    }
}
