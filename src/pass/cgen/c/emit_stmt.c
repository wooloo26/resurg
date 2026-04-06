#include "helpers.h"

// ── Branch / if helpers ────────────────────────────────────────────────

/** Emit an if/else branch body, optionally assigning the result to @p target. */
static void emit_branch(CGen *cgen, const HirNode *body, const char *target) {
    if (body->kind == HIR_BLOCK) {
        emit_block_stmts(cgen, body);
        if (body->block.result != NULL) {
            if (target != NULL) {
                const char *value = emit_expr(cgen, body->block.result);
                emit_line(cgen, "%s = %s;", target, value);
            } else {
                emit_stmt(cgen, body->block.result);
            }
        }
    } else {
        if (target != NULL) {
            const char *value = emit_expr(cgen, body);
            emit_line(cgen, "%s = %s;", target, value);
        } else {
            emit_stmt(cgen, body);
        }
    }
}

void emit_if(CGen *cgen, const HirNode *node, const char *target, bool is_else_if) {
    const char *cond_value = emit_expr(cgen, node->if_expr.cond);

    // Wrap in parentheses unless already parenthesized
    const char *wrapped = cond_value;
    if (cond_value[0] != '(') {
        wrapped = arena_sprintf(cgen->arena, "(%s)", cond_value);
    }
    if (is_else_if) {
        fprintf(cgen->output, "if %s {\n", wrapped);
    } else {
        emit_line(cgen, "if %s {", wrapped);
    }
    cgen->indent++;

    emit_branch(cgen, node->if_expr.then_body, target);
    cgen->indent--;

    if (node->if_expr.else_body != NULL) {
        if (node->if_expr.else_body->kind == HIR_IF) {
            emit_indent(cgen);
            fprintf(cgen->output, "} else ");
            emit_if(cgen, node->if_expr.else_body, target, true);
        } else {
            emit_line(cgen, "} else {");
            cgen->indent++;
            emit_branch(cgen, node->if_expr.else_body, target);
            cgen->indent--;
            emit_line(cgen, "}");
        }
    } else {
        emit_line(cgen, "}");
    }
}

void emit_block_stmts(CGen *cgen, const HirNode *block) {
    if (block == NULL || block->kind != HIR_BLOCK) {
        return;
    }
    for (int32_t i = 0; i < BUF_LEN(block->block.stmts); i++) {
        emit_stmt(cgen, block->block.stmts[i]);
    }
    // Note: block.result is NOT emitted here (caller handles it)
}

/** Emit all block stmts and the trailing result (if any) as a stmt. */
static void emit_block_body(CGen *cgen, const HirNode *block) {
    if (block == NULL || block->kind != HIR_BLOCK) {
        return;
    }
    emit_block_stmts(cgen, block);
    if (block->block.result != NULL) {
        emit_stmt(cgen, block->block.result);
    }
}

// ── Per-node stmt emitters ────────────────────────────────────────

static void emit_var_decl_stmt(CGen *cgen, const HirNode *node) {
    const Type *type = node->var_decl.var_type;
    if (type == NULL && node->var_decl.init != NULL) {
        type = node->var_decl.init->type;
    }
    if (type == NULL) {
        type = &TYPE_I32_INST;
    }

    // unit/never-typed vars have no C representation; emit init
    // (if any) as a bare expr for its side effects.
    if (type->kind == TYPE_UNIT || type->kind == TYPE_NEVER) {
        if (node->var_decl.init != NULL) {
            emit_stmt(cgen, node->var_decl.init);
        }
        return;
    }

    const char *c_name = node->var_decl.sym->mangled_name;
    if (node->var_decl.init != NULL) {
        const char *value = emit_expr(cgen, node->var_decl.init);
        emit_line(cgen, "%s %s = %s;", c_type_for(cgen, type), c_name, value);
    } else {
        emit_line(cgen, "%s %s;", c_type_for(cgen, type), c_name);
    }
}

/** Resolve an assignment target to a C lvalue expr. */
static const char *resolve_assign_target(CGen *cgen, const HirNode *target) {
    if (target->kind == HIR_VAR_REF) {
        return target->var_ref.sym->mangled_name;
    }
    if (target->kind == HIR_IDX) {
        const char *obj = emit_expr(cgen, target->idx_access.object);
        const char *idx = emit_expr(cgen, target->idx_access.idx);
        const Type *obj_type = target->idx_access.object->type;
        if (obj_type != NULL && obj_type->kind == TYPE_SLICE) {
            const char *elem_c = c_type_for(cgen, target->type);
            return arena_sprintf(cgen->arena, "((%s*)%s.data)[%s]", elem_c, obj, idx);
        }
        return arena_sprintf(cgen->arena, "%s._data[%s]", obj, idx);
    }
    if (target->kind == HIR_STRUCT_FIELD_ACCESS) {
        const char *obj = resolve_assign_target(cgen, target->struct_field_access.object);
        if (target->struct_field_access.via_ptr) {
            return arena_sprintf(cgen->arena, "%s->%s", obj, target->struct_field_access.field);
        }
        return arena_sprintf(cgen->arena, "%s.%s", obj, target->struct_field_access.field);
    }
    if (target->kind == HIR_DEREF) {
        const char *inner = emit_expr(cgen, target->deref.operand);
        return arena_sprintf(cgen->arena, "(*%s)", inner);
    }
    return emit_expr(cgen, target);
}

static void emit_assign_stmt(CGen *cgen, const HirNode *node) {
    const char *target = resolve_assign_target(cgen, node->assign.target);
    const char *value = emit_expr(cgen, node->assign.value);
    emit_line(cgen, "%s = %s;", target, value);
}

static void emit_loop_stmt(CGen *cgen, const HirNode *node) {
    emit_line(cgen, "while (1) {");
    cgen->indent++;
    emit_block_body(cgen, node->loop.body);
    cgen->indent--;
    emit_line(cgen, "}");
}

static void emit_return_stmt(CGen *cgen, const HirNode *node) {
    if (node->return_stmt.value != NULL) {
        const char *value = emit_expr(cgen, node->return_stmt.value);
        emit_line(cgen, "return %s;", value);
    } else {
        emit_line(cgen, "return;");
    }
}

// ── Statement dispatch ─────────────────────────────────────────────────

void emit_stmt(CGen *cgen, const HirNode *node) {
    if (node == NULL) {
        return;
    }
    switch (node->kind) {
    case HIR_VAR_DECL:
        emit_var_decl_stmt(cgen, node);
        break;
    case HIR_ASSIGN:
        emit_assign_stmt(cgen, node);
        break;
    case HIR_BREAK:
        emit_line(cgen, "break;");
        break;
    case HIR_CONTINUE:
        emit_line(cgen, "continue;");
        break;
    case HIR_RETURN:
        if (cgen->in_deferred_fn) {
            if (node->return_stmt.value != NULL) {
                const char *value = emit_expr(cgen, node->return_stmt.value);
                emit_line(cgen, "_rsg_result = %s;", value);
            }
            emit_line(cgen, "goto _rsg_cleanup;");
        } else {
            emit_return_stmt(cgen, node);
        }
        break;
    case HIR_LOOP:
        emit_loop_stmt(cgen, node);
        break;
    case HIR_DEFER:
        BUF_PUSH(cgen->defer_bodies, node);
        break;
    case HIR_IF:
        emit_if(cgen, node, NULL, false);
        break;
    case HIR_BLOCK: {
        emit_line(cgen, "{");
        cgen->indent++;
        emit_block_body(cgen, node);
        cgen->indent--;
        emit_line(cgen, "}");
        break;
    }
    default: {
        const char *value = emit_expr(cgen, node);
        emit_line(cgen, "%s;", value);
        break;
    }
    }
}
