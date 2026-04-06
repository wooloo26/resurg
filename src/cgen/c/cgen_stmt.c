#include "_cgen.h"

// ── Branch / if helpers ────────────────────────────────────────────────

/** Emit an if/else branch body, optionally assigning the result to @p target. */
static void emit_branch(CGen *cgen, const TTNode *body, const char *target) {
    if (body->kind == TT_BLOCK) {
        cgen_emit_block_stmts(cgen, body);
        if (body->block.result != NULL) {
            if (target != NULL) {
                const char *value = cgen_emit_expr(cgen, body->block.result);
                cgen_emit_line(cgen, "%s = %s;", target, value);
            } else {
                cgen_emit_stmt(cgen, body->block.result);
            }
        }
    } else {
        if (target != NULL) {
            const char *value = cgen_emit_expr(cgen, body);
            cgen_emit_line(cgen, "%s = %s;", target, value);
        } else {
            cgen_emit_stmt(cgen, body);
        }
    }
}

void cgen_emit_if(CGen *cgen, const TTNode *node, const char *target, bool is_else_if) {
    const char *cond_value = cgen_emit_expr(cgen, node->if_expr.cond);

    // Wrap in parentheses unless already parenthesized
    const char *wrapped = cond_value;
    if (cond_value[0] != '(') {
        wrapped = arena_sprintf(cgen->arena, "(%s)", cond_value);
    }
    if (is_else_if) {
        fprintf(cgen->output, "if %s {\n", wrapped);
    } else {
        cgen_emit_line(cgen, "if %s {", wrapped);
    }
    cgen->indent++;

    emit_branch(cgen, node->if_expr.then_body, target);
    cgen->indent--;

    if (node->if_expr.else_body != NULL) {
        if (node->if_expr.else_body->kind == TT_IF) {
            cgen_emit_indent(cgen);
            fprintf(cgen->output, "} else ");
            cgen_emit_if(cgen, node->if_expr.else_body, target, true);
        } else {
            cgen_emit_line(cgen, "} else {");
            cgen->indent++;
            emit_branch(cgen, node->if_expr.else_body, target);
            cgen->indent--;
            cgen_emit_line(cgen, "}");
        }
    } else {
        cgen_emit_line(cgen, "}");
    }
}

void cgen_emit_block_stmts(CGen *cgen, const TTNode *block) {
    if (block == NULL || block->kind != TT_BLOCK) {
        return;
    }
    for (int32_t i = 0; i < BUF_LEN(block->block.stmts); i++) {
        cgen_emit_stmt(cgen, block->block.stmts[i]);
    }
    // Note: block.result is NOT emitted here (caller handles it)
}

/** Emit all block stmts and the trailing result (if any) as a stmt. */
static void emit_block_body(CGen *cgen, const TTNode *block) {
    if (block == NULL || block->kind != TT_BLOCK) {
        return;
    }
    cgen_emit_block_stmts(cgen, block);
    if (block->block.result != NULL) {
        cgen_emit_stmt(cgen, block->block.result);
    }
}

// ── Per-node stmt emitters ────────────────────────────────────────

static void emit_var_decl_stmt(CGen *cgen, const TTNode *node) {
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
            cgen_emit_stmt(cgen, node->var_decl.init);
        }
        return;
    }

    const char *c_name = node->var_decl.sym->mangled_name;
    if (node->var_decl.init != NULL) {
        const char *value = cgen_emit_expr(cgen, node->var_decl.init);
        cgen_emit_line(cgen, "%s %s = %s;", cgen_c_type_for(cgen, type), c_name, value);
    } else {
        cgen_emit_line(cgen, "%s %s;", cgen_c_type_for(cgen, type), c_name);
    }
}

/** Resolve an assignment target to a C lvalue expr. */
static const char *resolve_assign_target(CGen *cgen, const TTNode *target) {
    if (target->kind == TT_VAR_REF) {
        return target->var_ref.sym->mangled_name;
    }
    if (target->kind == TT_IDX) {
        const char *obj = cgen_emit_expr(cgen, target->idx_access.object);
        const char *idx = cgen_emit_expr(cgen, target->idx_access.idx);
        const Type *obj_type = target->idx_access.object->type;
        if (obj_type != NULL && obj_type->kind == TYPE_SLICE) {
            const char *elem_c = cgen_c_type_for(cgen, target->type);
            return arena_sprintf(cgen->arena, "((%s*)%s.data)[%s]", elem_c, obj, idx);
        }
        return arena_sprintf(cgen->arena, "%s._data[%s]", obj, idx);
    }
    if (target->kind == TT_STRUCT_FIELD_ACCESS) {
        const char *obj = resolve_assign_target(cgen, target->struct_field_access.object);
        if (target->struct_field_access.via_ptr) {
            return arena_sprintf(cgen->arena, "%s->%s", obj, target->struct_field_access.field);
        }
        return arena_sprintf(cgen->arena, "%s.%s", obj, target->struct_field_access.field);
    }
    if (target->kind == TT_DEREF) {
        const char *inner = cgen_emit_expr(cgen, target->deref.operand);
        return arena_sprintf(cgen->arena, "(*%s)", inner);
    }
    return cgen_emit_expr(cgen, target);
}

static void emit_assign_stmt(CGen *cgen, const TTNode *node) {
    const char *target = resolve_assign_target(cgen, node->assign.target);
    const char *value = cgen_emit_expr(cgen, node->assign.value);
    cgen_emit_line(cgen, "%s = %s;", target, value);
}

static void emit_loop_stmt(CGen *cgen, const TTNode *node) {
    cgen_emit_line(cgen, "while (1) {");
    cgen->indent++;
    emit_block_body(cgen, node->loop.body);
    cgen->indent--;
    cgen_emit_line(cgen, "}");
}

static void emit_return_stmt(CGen *cgen, const TTNode *node) {
    if (node->return_stmt.value != NULL) {
        const char *value = cgen_emit_expr(cgen, node->return_stmt.value);
        cgen_emit_line(cgen, "return %s;", value);
    } else {
        cgen_emit_line(cgen, "return;");
    }
}

// ── Statement dispatch ─────────────────────────────────────────────────

void cgen_emit_stmt(CGen *cgen, const TTNode *node) {
    if (node == NULL) {
        return;
    }
    switch (node->kind) {
    case TT_VAR_DECL:
        emit_var_decl_stmt(cgen, node);
        break;
    case TT_ASSIGN:
        emit_assign_stmt(cgen, node);
        break;
    case TT_BREAK:
        cgen_emit_line(cgen, "break;");
        break;
    case TT_CONTINUE:
        cgen_emit_line(cgen, "continue;");
        break;
    case TT_RETURN:
        if (cgen->in_deferred_fn) {
            if (node->return_stmt.value != NULL) {
                const char *value = cgen_emit_expr(cgen, node->return_stmt.value);
                cgen_emit_line(cgen, "_rsg_result = %s;", value);
            }
            cgen_emit_line(cgen, "goto _rsg_cleanup;");
        } else {
            emit_return_stmt(cgen, node);
        }
        break;
    case TT_LOOP:
        emit_loop_stmt(cgen, node);
        break;
    case TT_DEFER:
        BUF_PUSH(cgen->defer_bodies, node);
        break;
    case TT_IF:
        cgen_emit_if(cgen, node, NULL, false);
        break;
    case TT_BLOCK: {
        cgen_emit_line(cgen, "{");
        cgen->indent++;
        emit_block_body(cgen, node);
        cgen->indent--;
        cgen_emit_line(cgen, "}");
        break;
    }
    default: {
        const char *value = cgen_emit_expr(cgen, node);
        cgen_emit_line(cgen, "%s;", value);
        break;
    }
    }
}
