#include "_codegen.h"

// ── Branch / if helpers ────────────────────────────────────────────────

/** Emit an if/else branch body, optionally assigning the result to @p target. */
static void emit_branch(CodeGenerator *generator, const TtNode *body, const char *target) {
    if (body->kind == TT_BLOCK) {
        codegen_emit_block_statements(generator, body);
        if (body->block.result != NULL) {
            if (target != NULL) {
                const char *value = codegen_emit_expression(generator, body->block.result);
                codegen_emit_line(generator, "%s = %s;", target, value);
            } else {
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

void codegen_emit_if(CodeGenerator *generator, const TtNode *node, const char *target,
                     bool is_else_if) {
    const char *condition_value = codegen_emit_expression(generator, node->if_expression.condition);

    // Wrap in parentheses unless already parenthesized
    const char *wrapped = condition_value;
    if (condition_value[0] != '(') {
        wrapped = arena_sprintf(generator->arena, "(%s)", condition_value);
    }
    if (is_else_if) {
        fprintf(generator->output, "if %s {\n", wrapped);
    } else {
        codegen_emit_line(generator, "if %s {", wrapped);
    }
    generator->indent++;

    emit_branch(generator, node->if_expression.then_body, target);
    generator->indent--;

    if (node->if_expression.else_body != NULL) {
        if (node->if_expression.else_body->kind == TT_IF) {
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

void codegen_emit_block_statements(CodeGenerator *generator, const TtNode *block) {
    if (block == NULL || block->kind != TT_BLOCK) {
        return;
    }
    for (int32_t i = 0; i < BUFFER_LENGTH(block->block.statements); i++) {
        codegen_emit_statement(generator, block->block.statements[i]);
    }
    // Note: block.result is NOT emitted here (caller handles it)
}

/** Emit all block statements and the trailing result (if any) as a statement. */
static void emit_block_body(CodeGenerator *generator, const TtNode *block) {
    if (block == NULL || block->kind != TT_BLOCK) {
        return;
    }
    codegen_emit_block_statements(generator, block);
    if (block->block.result != NULL) {
        codegen_emit_statement(generator, block->block.result);
    }
}

// ── Per-node statement emitters ────────────────────────────────────────

/**
 * Detect the for-loop pattern desugared by lowering:
 *   Block {
 *     VarDecl _end = end_expr
 *     VarDecl iter = start_expr
 *     Loop { Block {
 *       If(iter >= _end) { Break }
 *       ... user body ...
 *       Assign(iter = iter + 1)
 *     }}
 *   }
 *
 * Returns true and fills out parameters if the pattern matches.
 */
static bool detect_for_loop_pattern(const TtNode *block, const TtNode **out_end_init,
                                    const char **out_iter_name, const TtNode **out_start_init,
                                    const TtNode **out_end_var_ref, const TtNode **out_loop_body,
                                    int32_t *out_body_start, int32_t *out_body_end) {
    if (block->kind != TT_BLOCK) {
        return false;
    }
    int32_t count = BUFFER_LENGTH(block->block.statements);
    if (count != 3) {
        return false;
    }

    const TtNode *end_decl = block->block.statements[0];
    const TtNode *iter_decl = block->block.statements[1];
    const TtNode *loop_node = block->block.statements[2];

    if (end_decl->kind != TT_VARIABLE_DECLARATION || iter_decl->kind != TT_VARIABLE_DECLARATION ||
        loop_node->kind != TT_LOOP) {
        return false;
    }

    // Check the loop body is a block
    const TtNode *loop_body = loop_node->loop.body;
    if (loop_body == NULL || loop_body->kind != TT_BLOCK) {
        return false;
    }

    int32_t stmt_count = BUFFER_LENGTH(loop_body->block.statements);
    if (stmt_count < 2) {
        return false;
    }

    // First statement: if (iter >= end) { break }
    const TtNode *guard = loop_body->block.statements[0];
    if (guard->kind != TT_IF || guard->if_expression.else_body != NULL) {
        return false;
    }
    const TtNode *cond = guard->if_expression.condition;
    if (cond->kind != TT_BINARY || cond->binary.op != TOKEN_GREATER_EQUAL) {
        return false;
    }

    // Last statement: iter = iter + 1
    const TtNode *last = loop_body->block.statements[stmt_count - 1];
    if (last->kind != TT_ASSIGN) {
        return false;
    }
    if (last->assign.value->kind != TT_BINARY || last->assign.value->binary.op != TOKEN_PLUS) {
        return false;
    }

    *out_end_init = end_decl->variable_declaration.initializer;
    *out_iter_name = iter_decl->variable_declaration.name;
    *out_start_init = iter_decl->variable_declaration.initializer;
    *out_end_var_ref = cond->binary.right;
    *out_loop_body = loop_body;
    *out_body_start = 1;            // skip the guard
    *out_body_end = stmt_count - 1; // skip the increment
    return true;
}

/** Emit a detected for-loop pattern as a C for statement. */
static void emit_for_loop_pattern(CodeGenerator *generator, const TtNode *start_init,
                                  const TtNode *end_init, const char *iter_name,
                                  const TtNode *loop_body, int32_t body_start, int32_t body_end) {
    const char *start_str = codegen_emit_expression(generator, start_init);
    const char *end_str = codegen_emit_expression(generator, end_init);
    const char *c_iter_name = codegen_variable_define(generator, iter_name);

    codegen_emit_line(generator, "for (int32_t %s = %s; %s < %s; %s++) {", c_iter_name, start_str,
                      c_iter_name, end_str, c_iter_name);
    generator->indent++;

    for (int32_t i = body_start; i < body_end; i++) {
        codegen_emit_statement(generator, loop_body->block.statements[i]);
    }
    if (loop_body->block.result != NULL) {
        codegen_emit_statement(generator, loop_body->block.result);
    }

    generator->indent--;
    codegen_emit_line(generator, "}");
}

static void emit_variable_declaration_statement(CodeGenerator *generator, const TtNode *node) {
    const Type *type = node->variable_declaration.var_type;
    if (type == NULL && node->variable_declaration.initializer != NULL) {
        type = node->variable_declaration.initializer->type;
    }
    if (type == NULL) {
        type = &TYPE_I32_INSTANCE;
    }
    const char *c_name = codegen_variable_define(generator, node->variable_declaration.name);
    const char *value = codegen_emit_expression(generator, node->variable_declaration.initializer);
    codegen_emit_line(generator, "%s %s = %s;", codegen_c_type_for(generator, type), c_name, value);
}

static void emit_expression_statement_body(CodeGenerator *generator, const TtNode *node) {
    const TtNode *expression = node->expression_statement.expression;
    switch (expression->kind) {
    case TT_IF:
        codegen_emit_if(generator, expression, NULL, false);
        break;
    case TT_ASSIGN:
        codegen_emit_statement(generator, expression);
        break;
    default: {
        const char *value = codegen_emit_expression(generator, expression);
        codegen_emit_line(generator, "%s;", value);
        break;
    }
    }
}

/** Resolve an assignment target to a C lvalue expression. */
static const char *resolve_assign_target(CodeGenerator *generator, const TtNode *target) {
    if (target->kind == TT_VARIABLE_REFERENCE) {
        return codegen_variable_lookup(generator,
                                       tt_symbol_name(target->variable_reference.symbol));
    }
    if (target->kind == TT_INDEX) {
        const char *obj = codegen_emit_expression(generator, target->index_access.object);
        const char *idx = codegen_emit_expression(generator, target->index_access.index);
        return arena_sprintf(generator->arena, "%s._data[%s]", obj, idx);
    }
    return codegen_emit_expression(generator, target);
}

static void emit_assign_statement(CodeGenerator *generator, const TtNode *node) {
    const char *target = resolve_assign_target(generator, node->assign.target);
    const char *value = codegen_emit_expression(generator, node->assign.value);
    codegen_emit_line(generator, "%s = %s;", target, value);
}

static void emit_loop_statement(CodeGenerator *generator, const TtNode *node) {
    codegen_emit_line(generator, "while (1) {");
    generator->indent++;
    emit_block_body(generator, node->loop.body);
    generator->indent--;
    codegen_emit_line(generator, "}");
}

static void emit_return_statement(CodeGenerator *generator, const TtNode *node) {
    if (node->return_statement.value != NULL) {
        const char *value = codegen_emit_expression(generator, node->return_statement.value);
        codegen_emit_line(generator, "return %s;", value);
    } else {
        codegen_emit_line(generator, "return;");
    }
}

// ── Statement dispatch ─────────────────────────────────────────────────

void codegen_emit_statement(CodeGenerator *generator, const TtNode *node) {
    if (node == NULL) {
        return;
    }
    switch (node->kind) {
    case TT_VARIABLE_DECLARATION:
        emit_variable_declaration_statement(generator, node);
        break;
    case TT_EXPRESSION_STATEMENT:
        emit_expression_statement_body(generator, node);
        break;
    case TT_ASSIGN:
        emit_assign_statement(generator, node);
        break;
    case TT_BREAK:
        codegen_emit_line(generator, "break;");
        break;
    case TT_CONTINUE:
        codegen_emit_line(generator, "continue;");
        break;
    case TT_RETURN:
        emit_return_statement(generator, node);
        break;
    case TT_LOOP:
        emit_loop_statement(generator, node);
        break;
    case TT_IF:
        codegen_emit_if(generator, node, NULL, false);
        break;
    case TT_BLOCK: {
        // Check for desugared for-loop pattern
        const TtNode *end_init, *start_init, *end_ref, *loop_body;
        const char *iter_name;
        int32_t bs, be;
        if (detect_for_loop_pattern(node, &end_init, &iter_name, &start_init, &end_ref, &loop_body,
                                    &bs, &be)) {
            emit_for_loop_pattern(generator, start_init, end_init, iter_name, loop_body, bs, be);
        } else {
            codegen_emit_line(generator, "{");
            generator->indent++;
            emit_block_body(generator, node);
            generator->indent--;
            codegen_emit_line(generator, "}");
        }
        break;
    }
    default: {
        const char *value = codegen_emit_expression(generator, node);
        codegen_emit_line(generator, "%s;", value);
        break;
    }
    }
}
