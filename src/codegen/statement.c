#include "codegen/_codegen.h"

// ── Branch / if helpers ────────────────────────────────────────────────

/** Emit an if/else branch body, optionally assigning the result to @p target. */
static void emit_branch(CodeGenerator *generator, const ASTNode *body, const char *target) {
    if (body->kind == NODE_BLOCK) {
        if (target != NULL) {
            for (int32_t i = 0; i < BUFFER_LENGTH(body->block.statements); i++) {
                codegen_emit_statement(generator, body->block.statements[i]);
            }
            if (body->block.result != NULL) {
                const char *value = codegen_emit_expression(generator, body->block.result);
                codegen_emit_line(generator, "%s = %s;", target, value);
            }
        } else {
            codegen_emit_block_statements(generator, body);
            if (body->block.result != NULL) {
                codegen_emit_statement(generator, body->block.result);
            }
        }
    } else {
        if (target != NULL) {
            const char *value = codegen_emit_expression(generator, body);
            codegen_emit_line(generator, "%s = %s;", target, value);
        } else {
            codegen_emit_statement(generator, body);
        }
    }
}

void codegen_emit_if(CodeGenerator *generator, const ASTNode *node, const char *target, bool is_else_if) {
    const char *condition_value = codegen_emit_expression(generator, node->if_expression.condition);

    // Wrap in parentheses unless already parenthesized
    const char *wrapped =
        (condition_value[0] == '(') ? condition_value : arena_sprintf(generator->arena, "(%s)", condition_value);
    if (is_else_if) {
        fprintf(generator->output, "if %s {\n", wrapped);
    } else {
        codegen_emit_line(generator, "if %s {", wrapped);
    }
    generator->indent++;

    emit_branch(generator, node->if_expression.then_body, target);
    generator->indent--;

    if (node->if_expression.else_body != NULL) {
        if (node->if_expression.else_body->kind == NODE_IF) {
            codegen_emit_indent(generator);
            fprintf(generator->output, "} else ");
            codegen_emit_if(generator, node->if_expression.else_body, target, true);
        } else {
            codegen_emit_line(generator, "} else {");
            generator->indent++;
            emit_branch(generator, node->if_expression.else_body, target);
            generator->indent--;
            codegen_emit_line(generator, "}");
        }
    } else {
        codegen_emit_line(generator, "}");
    }
}

void codegen_emit_block_statements(CodeGenerator *generator, const ASTNode *block) {
    if (block == NULL || block->kind != NODE_BLOCK) {
        return;
    }
    for (int32_t i = 0; i < BUFFER_LENGTH(block->block.statements); i++) {
        codegen_emit_statement(generator, block->block.statements[i]);
    }
    // Note: block.result is NOT emitted here (caller handles it)
}

/** Emit all block statements and the trailing result (if any) as a statement. */
static void emit_block_body(CodeGenerator *generator, const ASTNode *block) {
    if (block == NULL || block->kind != NODE_BLOCK) {
        return;
    }
    codegen_emit_block_statements(generator, block);
    if (block->block.result != NULL) {
        codegen_emit_statement(generator, block->block.result);
    }
}

// ── Per-node statement emitters ────────────────────────────────────────

static void emit_variable_declaration_statement(CodeGenerator *generator, const ASTNode *node) {
    const Type *type = node->type;
    if (type == NULL) {
        type = node->variable_declaration.initializer != NULL ? node->variable_declaration.initializer->type : NULL;
    }
    if (type == NULL && node->variable_declaration.type.kind == AST_TYPE_NAME) {
        type = type_from_name(node->variable_declaration.type.name);
    }
    if (type == NULL) {
        type = &TYPE_I32_INSTANCE;
    }
    const char *c_name = codegen_variable_define(generator, node->variable_declaration.name);
    const char *value = codegen_emit_expression(generator, node->variable_declaration.initializer);
    codegen_emit_line(generator, "%s %s = %s;", codegen_c_type_for(generator, type), c_name, value);
}

static void emit_expression_statement_body(CodeGenerator *generator, const ASTNode *node) {
    const ASTNode *expression = node->expression_statement.expression;
    switch (expression->kind) {
    case NODE_IF:
        codegen_emit_if(generator, expression, NULL, false);
        break;
    case NODE_ASSIGN:
    case NODE_COMPOUND_ASSIGN:
        codegen_emit_statement(generator, expression);
        break;
    default: {
        const char *value = codegen_emit_expression(generator, expression);
        codegen_emit_line(generator, "%s;", value);
        break;
    }
    }
}

static void emit_assign_statement(CodeGenerator *generator, const ASTNode *node) {
    const char *target;
    if (node->assign.target->kind == NODE_IDENTIFIER) {
        target = codegen_variable_lookup(generator, node->assign.target->identifier.name);
    } else if (node->assign.target->kind == NODE_INDEX) {
        const char *obj = codegen_emit_expression(generator, node->assign.target->index_access.object);
        const char *idx = codegen_emit_expression(generator, node->assign.target->index_access.index);
        target = arena_sprintf(generator->arena, "%s._data[%s]", obj, idx);
    } else {
        target = codegen_emit_expression(generator, node->assign.target);
    }
    const char *value = codegen_emit_expression(generator, node->assign.value);
    codegen_emit_line(generator, "%s = %s;", target, value);
}

static void emit_compound_assign_statement(CodeGenerator *generator, const ASTNode *node) {
    const char *target;
    if (node->compound_assign.target->kind == NODE_IDENTIFIER) {
        target = codegen_variable_lookup(generator, node->compound_assign.target->identifier.name);
    } else {
        target = codegen_emit_expression(generator, node->compound_assign.target);
    }
    const char *value = codegen_emit_expression(generator, node->compound_assign.value);
    codegen_emit_line(generator, "%s %s %s;", target, codegen_c_compound_operator(node->compound_assign.op), value);
}

static void emit_loop_statement(CodeGenerator *generator, const ASTNode *node) {
    codegen_emit_line(generator, "while (1) {");
    generator->indent++;
    emit_block_body(generator, node->loop.body);
    generator->indent--;
    codegen_emit_line(generator, "}");
}

static void emit_for_statement(CodeGenerator *generator, const ASTNode *node) {
    const char *start = codegen_emit_expression(generator, node->for_loop.start);
    const char *end = codegen_emit_expression(generator, node->for_loop.end);
    const char *c_variable_name = codegen_variable_define(generator, node->for_loop.variable_name);
    codegen_emit_line(generator, "for (int32_t %s = %s; %s < %s; %s++) {", c_variable_name, start, c_variable_name, end,
                      c_variable_name);
    generator->indent++;
    emit_block_body(generator, node->for_loop.body);
    generator->indent--;
    codegen_emit_line(generator, "}");
}

// ── Statement dispatch ─────────────────────────────────────────────────

void codegen_emit_statement(CodeGenerator *generator, const ASTNode *node) {
    if (node == NULL) {
        return;
    }
    switch (node->kind) {
    case NODE_VARIABLE_DECLARATION:
        emit_variable_declaration_statement(generator, node);
        break;
    case NODE_EXPRESSION_STATEMENT:
        emit_expression_statement_body(generator, node);
        break;
    case NODE_ASSIGN:
        emit_assign_statement(generator, node);
        break;
    case NODE_COMPOUND_ASSIGN:
        emit_compound_assign_statement(generator, node);
        break;
    case NODE_BREAK:
        codegen_emit_line(generator, "break;");
        break;
    case NODE_CONTINUE:
        codegen_emit_line(generator, "continue;");
        break;
    case NODE_LOOP:
        emit_loop_statement(generator, node);
        break;
    case NODE_FOR:
        emit_for_statement(generator, node);
        break;
    case NODE_IF:
        codegen_emit_if(generator, node, NULL, false);
        break;
    case NODE_BLOCK:
        codegen_emit_line(generator, "{");
        generator->indent++;
        emit_block_body(generator, node);
        generator->indent--;
        codegen_emit_line(generator, "}");
        break;
    default: {
        const char *value = codegen_emit_expression(generator, node);
        codegen_emit_line(generator, "%s;", value);
        break;
    }
    }
}
