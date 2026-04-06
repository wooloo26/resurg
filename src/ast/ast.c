#include "ast.h"

// Indentation helper for ast_dump.
static void print_indent(int32_t level) {
    for (int32_t i = 0; i < level; i++) {
        fprintf(stderr, "  ");
    }
}

// Per-node dump helpers - each prints one node kind and recurses.
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

static void dump_literal_node(const ASTNode *node) {
    fprintf(stderr, "Literal(");
    switch (node->literal.kind) {
    case LITERAL_BOOL:
        fprintf(stderr, "%s", node->literal.boolean_value ? "true" : "false");
        break;
    case LITERAL_I8:
    case LITERAL_I16:
    case LITERAL_I32:
    case LITERAL_I64:
    case LITERAL_I128:
    case LITERAL_ISIZE:
        fprintf(stderr, "%lld", (long long)(int64_t)node->literal.integer_value);
        break;
    case LITERAL_U8:
    case LITERAL_U16:
    case LITERAL_U32:
    case LITERAL_U64:
    case LITERAL_U128:
    case LITERAL_USIZE:
        fprintf(stderr, "%llu", (unsigned long long)node->literal.integer_value);
        break;
    case LITERAL_F32:
    case LITERAL_F64:
        fprintf(stderr, "%g", node->literal.float64_value);
        break;
    case LITERAL_CHAR:
        if (node->literal.char_value < 128) {
            fprintf(stderr, "'%c'", (char)node->literal.char_value);
        } else {
            fprintf(stderr, "'\\u{%04X}'", node->literal.char_value);
        }
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
    fprintf(stderr, "For(%s", node->for_loop.variable_name);
    if (node->for_loop.index_name != NULL) {
        fprintf(stderr, ", %s", node->for_loop.index_name);
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
    ASTNode *node = arena_alloc_zero(arena, sizeof(ASTNode));
    node->kind = kind;
    node->location = location;
    node->type = NULL;
    return node;
}

static void dump_unary_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "Unary(%s)\n", token_kind_string(node->unary.op));
    ast_dump(node->unary.operand, level + 1);
}

static void dump_binary_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "Binary(%s)\n", token_kind_string(node->binary.op));
    ast_dump(node->binary.left, level + 1);
    ast_dump(node->binary.right, level + 1);
}

static void dump_assign_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "Assign\n");
    ast_dump(node->assign.target, level + 1);
    ast_dump(node->assign.value, level + 1);
}

static void dump_compound_assign_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "CompoundAssign(%s)\n", token_kind_string(node->compound_assign.op));
    ast_dump(node->compound_assign.target, level + 1);
    ast_dump(node->compound_assign.value, level + 1);
}

static void dump_array_literal_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "ArrayLiteral(%d)\n", node->array_literal.size);
    for (int32_t i = 0; i < BUFFER_LENGTH(node->array_literal.elements); i++) {
        ast_dump(node->array_literal.elements[i], level + 1);
    }
}

static void dump_tuple_literal_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "TupleLiteral\n");
    for (int32_t i = 0; i < BUFFER_LENGTH(node->tuple_literal.elements); i++) {
        ast_dump(node->tuple_literal.elements[i], level + 1);
    }
}

static void dump_struct_declaration_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "StructDecl(%s)\n", node->struct_declaration.name);
    for (int32_t i = 0; i < BUFFER_LENGTH(node->struct_declaration.methods); i++) {
        ast_dump(node->struct_declaration.methods[i], level + 1);
    }
}

static void dump_struct_literal_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "StructLiteral(%s)\n", node->struct_literal.name);
    for (int32_t i = 0; i < BUFFER_LENGTH(node->struct_literal.field_values); i++) {
        ast_dump(node->struct_literal.field_values[i], level + 1);
    }
}

static void dump_enum_declaration_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "EnumDecl(%s)\n", node->enum_declaration.name);
    for (int32_t i = 0; i < BUFFER_LENGTH(node->enum_declaration.methods); i++) {
        ast_dump(node->enum_declaration.methods[i], level + 1);
    }
}

static void dump_match_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "Match\n");
    ast_dump(node->match_expression.operand, level + 1);
    for (int32_t i = 0; i < BUFFER_LENGTH(node->match_expression.arms); i++) {
        print_indent(level + 1);
        fprintf(stderr, "Arm\n");
        ast_dump(node->match_expression.arms[i].body, level + 2);
    }
}

static void dump_enum_init_node(const ASTNode *node, int32_t level) {
    fprintf(stderr, "EnumInit(%s::%s)\n", node->enum_init.enum_name, node->enum_init.variant_name);
    for (int32_t i = 0; i < BUFFER_LENGTH(node->enum_init.arguments); i++) {
        ast_dump(node->enum_init.arguments[i], level + 1);
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
    case NODE_INDEX:
        fprintf(stderr, "Index\n");
        ast_dump(node->index_access.object, level + 1);
        ast_dump(node->index_access.index, level + 1);
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
    case NODE_ARRAY_LITERAL:
        dump_array_literal_node(node, level);
        break;
    case NODE_TUPLE_LITERAL:
        dump_tuple_literal_node(node, level);
        break;
    case NODE_TYPE_CONVERSION:
        fprintf(stderr, "TypeConversion\n");
        ast_dump(node->type_conversion.operand, level + 1);
        break;
    case NODE_TYPE_ALIAS:
        fprintf(stderr, "TypeAlias(%s)\n", node->type_alias.name);
        break;
    case NODE_STRUCT_DECLARATION:
        dump_struct_declaration_node(node, level);
        break;
    case NODE_STRUCT_LITERAL:
        dump_struct_literal_node(node, level);
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
    case NODE_ENUM_DECLARATION:
        dump_enum_declaration_node(node, level);
        break;
    case NODE_MATCH:
        dump_match_node(node, level);
        break;
    case NODE_ENUM_INIT:
        dump_enum_init_node(node, level);
        break;
    case NODE_RETURN:
        fprintf(stderr, "Return\n");
        if (node->return_statement.value != NULL) {
            ast_dump(node->return_statement.value, level + 1);
        }
        break;
    case NODE_WHILE:
        fprintf(stderr, "While\n");
        ast_dump(node->while_loop.condition, level + 1);
        ast_dump(node->while_loop.body, level + 1);
        break;
    case NODE_DEFER:
        fprintf(stderr, "Defer\n");
        ast_dump(node->defer_statement.body, level + 1);
        break;
    case NODE_SLICE_LITERAL:
        fprintf(stderr, "SliceLiteral(%d)\n", BUFFER_LENGTH(node->slice_literal.elements));
        for (int32_t i = 0; i < BUFFER_LENGTH(node->slice_literal.elements); i++) {
            ast_dump(node->slice_literal.elements[i], level + 1);
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
    }
}
