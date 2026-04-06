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

static void emit_variable_declaration_statement(CodeGenerator *generator, const TtNode *node) {
    const Type *type = node->variable_declaration.var_type;
    if (type == NULL && node->variable_declaration.initializer != NULL) {
        type = node->variable_declaration.initializer->type;
    }
    if (type == NULL) {
        type = &TYPE_I32_INSTANCE;
    }

    // unit/never-typed variables have no C representation; emit initializer
    // (if any) as a bare expression for its side effects.
    if (type->kind == TYPE_UNIT || type->kind == TYPE_NEVER) {
        if (node->variable_declaration.initializer != NULL) {
            codegen_emit_statement(generator, node->variable_declaration.initializer);
        }
        return;
    }

    const char *c_name = node->variable_declaration.symbol->mangled_name;
    if (node->variable_declaration.initializer != NULL) {
        const char *value =
            codegen_emit_expression(generator, node->variable_declaration.initializer);
        codegen_emit_line(generator, "%s %s = %s;", codegen_c_type_for(generator, type), c_name,
                          value);
    } else {
        codegen_emit_line(generator, "%s %s;", codegen_c_type_for(generator, type), c_name);
    }
}

/** Resolve an assignment target to a C lvalue expression. */
static const char *resolve_assign_target(CodeGenerator *generator, const TtNode *target) {
    if (target->kind == TT_VARIABLE_REFERENCE) {
        return target->variable_reference.symbol->mangled_name;
    }
    if (target->kind == TT_INDEX) {
        const char *obj = codegen_emit_expression(generator, target->index_access.object);
        const char *idx = codegen_emit_expression(generator, target->index_access.index);
        return arena_sprintf(generator->arena, "%s._data[%s]", obj, idx);
    }
    if (target->kind == TT_STRUCT_FIELD_ACCESS) {
        const char *obj = resolve_assign_target(generator, target->struct_field_access.object);
        if (target->struct_field_access.via_pointer) {
            return arena_sprintf(generator->arena, "%s->%s", obj,
                                 target->struct_field_access.field);
        }
        return arena_sprintf(generator->arena, "%s.%s", obj, target->struct_field_access.field);
    }
    if (target->kind == TT_DEREF) {
        const char *inner = codegen_emit_expression(generator, target->deref.operand);
        return arena_sprintf(generator->arena, "(*%s)", inner);
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
        if (generator->in_deferred_function) {
            if (node->return_statement.value != NULL) {
                const char *value =
                    codegen_emit_expression(generator, node->return_statement.value);
                codegen_emit_line(generator, "_rsg_result = %s;", value);
            }
            codegen_emit_line(generator, "goto _rsg_cleanup;");
        } else {
            emit_return_statement(generator, node);
        }
        break;
    case TT_LOOP:
        emit_loop_statement(generator, node);
        break;
    case TT_DEFER:
        BUFFER_PUSH(generator->defer_bodies, node);
        break;
    case TT_IF:
        codegen_emit_if(generator, node, NULL, false);
        break;
    case TT_BLOCK: {
        codegen_emit_line(generator, "{");
        generator->indent++;
        emit_block_body(generator, node);
        generator->indent--;
        codegen_emit_line(generator, "}");
        break;
    }
    default: {
        const char *value = codegen_emit_expression(generator, node);
        codegen_emit_line(generator, "%s;", value);
        break;
    }
    }
}
