#include "helpers.h"

// ── Fn body / decl ────────────────────────────────────────

/** Recursively check whether @p node (or any descendant) contains HIR_DEFER. */
static bool tree_has_defers(const HirNode *node) {
    if (node == NULL) {
        return false;
    }
    if (node->kind == HIR_DEFER) {
        return true;
    }
    switch (node->kind) {
    case HIR_BLOCK:
        for (int32_t i = 0; i < BUF_LEN(node->block.stmts); i++) {
            if (tree_has_defers(node->block.stmts[i])) {
                return true;
            }
        }
        return tree_has_defers(node->block.result);
    case HIR_IF:
        return tree_has_defers(node->if_expr.then_body) || tree_has_defers(node->if_expr.else_body);
    case HIR_LOOP:
        return tree_has_defers(node->loop.body);
    default:
        return false;
    }
}

/** Count every HIR_DEFER node reachable from @p node. */
static int32_t count_defers(const HirNode *node) {
    if (node == NULL) {
        return 0;
    }
    if (node->kind == HIR_DEFER) {
        return 1;
    }
    switch (node->kind) {
    case HIR_BLOCK: {
        int32_t n = 0;
        for (int32_t i = 0; i < BUF_LEN(node->block.stmts); i++) {
            n += count_defers(node->block.stmts[i]);
        }
        n += count_defers(node->block.result);
        return n;
    }
    case HIR_IF:
        return count_defers(node->if_expr.then_body) + count_defers(node->if_expr.else_body);
    case HIR_LOOP:
        return count_defers(node->loop.body);
    default:
        return 0;
    }
}

/**
 * Emit defer bodies in LIFO order, guarded by their activation flags.
 * When @p has_panic_frame is true, adds panic frame pop + repanic logic.
 * When @p panic_result_expr is non-NULL, re-reads the result expr after panic
 * recovery (defers may have modified the return variable).
 */
static void emit_deferred_cleanup(CGen *cgen, const Type *return_type, bool has_panic_frame,
                                  const char *panic_result_expr) {
    emit_line(cgen, RSG_INTERNAL_CLEANUP ":;");
    if (has_panic_frame) {
        emit_line(cgen, "%s();", cgen->abi->panic_pop);
    }
    for (int32_t i = BUF_LEN(cgen->defer_bodies) - 1; i >= 0; i--) {
        const HirNode *defer_node = cgen->defer_bodies[i];
        if (defer_node->defer_stmt.body != NULL) {
            emit_line(cgen, "if (" RSG_INTERNAL_DEFER "%d) {", i);
            cgen->indent++;
            emit_stmt(cgen, defer_node->defer_stmt.body);
            cgen->indent--;
            emit_line(cgen, "}");
        }
    }
    if (has_panic_frame) {
        emit_line(cgen, "if (" RSG_INTERNAL_PANICKED " && %s()) { %s(); }", cgen->abi->is_panicking,
                  cgen->abi->repanic);
    }
    if (return_type != NULL && return_type->kind != TYPE_UNIT && return_type->kind != TYPE_NEVER) {
        // After panic recovery, defers may have modified the return variable.
        // Re-read the trailing result expression to pick up those changes.
        if (has_panic_frame && panic_result_expr != NULL) {
            emit_line(cgen, "if (" RSG_INTERNAL_PANICKED ") { " RSG_INTERNAL_RESULT " = %s; }",
                      panic_result_expr);
        }
        emit_line(cgen, "return " RSG_INTERNAL_RESULT ";");
    }
}

/** Emit a return or defer-cleanup goto for a computed result. */
static void emit_return_or_defer(CGen *cgen, const char *result, bool has_defers) {
    if (has_defers) {
        emit_line(cgen, RSG_INTERNAL_RESULT " = %s;", result);
        emit_line(cgen, "goto " RSG_INTERNAL_CLEANUP ";");
    } else {
        emit_line(cgen, "return %s;", result);
    }
}

/** Emit a result expression evaluated only for side effects. */
static void emit_discard_result(CGen *cgen, const HirNode *result_node) {
    if (result_node->kind == HIR_CALL) {
        const char *result = emit_expr(cgen, result_node);
        emit_line(cgen, "%s;", result);
    } else if (result_node->kind == HIR_IF) {
        emit_if(cgen, result_node, NULL, false);
    } else {
        const char *result = emit_expr(cgen, result_node);
        emit_line(cgen, "(void)%s;", result);
    }
}

/**
 * Emit the body of a fn: block stmts, trailing result
 * expr, and an implicit `return 0;` for main.
 */
static void emit_fn_body(CGen *cgen, const HirNode *fn_node) {
    const HirNode *body = fn_node->fn_decl.body;
    const Type *return_type = fn_node->fn_decl.return_type;
    bool is_unit =
        return_type == NULL || return_type->kind == TYPE_UNIT || return_type->kind == TYPE_NEVER;
    bool is_main = strcmp(fn_node->fn_decl.name, "main") == 0;
    bool has_defers = tree_has_defers(body);

    // Save and set defer state
    const HirNode **saved_defers = cgen->defer_bodies;
    bool saved_in_deferred = cgen->in_deferred_fn;
    int32_t saved_defer_counter = cgen->defer_counter;
    cgen->defer_bodies = NULL;
    cgen->in_deferred_fn = has_defers;
    cgen->defer_counter = 0;

    if (has_defers && !is_unit && !is_main) {
        emit_line(cgen, "%s " RSG_INTERNAL_RESULT ";", c_type_for(cgen, return_type));
    }

    // Emit volatile boolean activation flags for every defer in this function
    if (has_defers) {
        int32_t defer_count = count_defers(body);
        for (int32_t i = 0; i < defer_count; i++) {
            emit_line(cgen, "volatile bool " RSG_INTERNAL_DEFER "%d = false;", i);
        }
    }

    // Push a panic frame so defers run on panic (setjmp/longjmp)
    if (has_defers) {
        emit_line(cgen, "RsgPanicFrame " RSG_INTERNAL_PANIC_FRAME ";");
        emit_line(cgen, "%s(&" RSG_INTERNAL_PANIC_FRAME ");", cgen->abi->panic_push);
        emit_line(cgen, "volatile bool " RSG_INTERNAL_PANICKED " = false;");
        emit_line(cgen, "if (setjmp(" RSG_INTERNAL_PANIC_FRAME ".env) != 0) {");
        cgen->indent++;
        emit_line(cgen, RSG_INTERNAL_PANICKED " = true;");
        emit_line(cgen, "goto " RSG_INTERNAL_CLEANUP ";");
        cgen->indent--;
        emit_line(cgen, "}");
    }

    // Track trailing result expr for panic recovery re-read
    const char *panic_result_expr = NULL;

    if (body != NULL && body->kind == HIR_BLOCK) {
        // Block body with optional trailing result
        emit_block_stmts(cgen, body);

        if (body->block.result != NULL) {
            const HirNode *result_node = body->block.result;
            if (result_node->kind == HIR_ASSIGN) {
                emit_stmt(cgen, result_node);
            } else if (!is_unit && !is_main) {
                const char *result_c = emit_expr(cgen, result_node);
                if (result_node->type != NULL && result_node->type->kind == TYPE_NEVER) {
                    // never-typed exprs (e.g. panic()) are void in C — emit as stmt
                    emit_line(cgen, "%s;", result_c);
                } else {
                    if (has_defers) {
                        panic_result_expr = result_c;
                    }
                    emit_return_or_defer(cgen, result_c, has_defers);
                }
            } else {
                emit_discard_result(cgen, result_node);
            }
        }
    } else if (body != NULL) {
        // Expression body (fn foo() = expr)
        if (!is_unit && !is_main) {
            const char *result_c = emit_expr(cgen, body);
            if (body->type != NULL && body->type->kind == TYPE_NEVER) {
                emit_line(cgen, "%s;", result_c);
            } else {
                if (has_defers) {
                    panic_result_expr = result_c;
                }
                emit_return_or_defer(cgen, result_c, has_defers);
            }
        } else {
            emit_line(cgen, "(void)%s;", emit_expr(cgen, body));
        }
    }

    if (has_defers) {
        emit_deferred_cleanup(cgen, is_unit ? NULL : return_type, has_defers, panic_result_expr);
    }

    // Restore defer state
    cgen->defer_bodies = saved_defers;
    cgen->in_deferred_fn = saved_in_deferred;
    cgen->defer_counter = saved_defer_counter;
}

/** Emit the fn sig: return-type name(params). */
static void emit_fn_sig(CGen *cgen, const HirNode *node) {
    const char *prefix = node->fn_decl.is_pub ? "" : "static ";
    const char *return_type = c_type_for(cgen, node->fn_decl.return_type);
    const char *fn_name = node->fn_decl.sym->mangled_name;

    emit_indent(cgen);
    fprintf(cgen->output, "%s%s %s(", prefix, return_type, fn_name);

    int32_t param_count = BUF_LEN(node->fn_decl.params);
    // Count visible (non-unit, non-recv-unit) params for the C signature
    int32_t visible_count = 0;
    for (int32_t i = 0; i < param_count; i++) {
        const HirNode *p = node->fn_decl.params[i];
        const Type *pt = p->param.param_type;
        if (pt != NULL && pt->kind == TYPE_UNIT && !p->param.is_recv) {
            continue;
        }
        visible_count++;
    }
    if (visible_count == 0) {
        fprintf(cgen->output, "void");
    } else {
        bool first = true;
        for (int32_t i = 0; i < param_count; i++) {
            const HirNode *param = node->fn_decl.params[i];
            const Type *param_type = param->param.param_type;
            if (param_type == NULL) {
                param_type = &TYPE_I32_INST;
            }
            // Skip unit-typed non-receiver params (void cannot be a C param)
            if (param_type->kind == TYPE_UNIT && !param->param.is_recv) {
                continue;
            }
            if (!first) {
                fprintf(cgen->output, ", ");
            }
            first = false;
            if (param->param.is_recv) {
                if (!param->param.is_ptr_recv) {
                    // Value recv: pass by value (copy)
                    fprintf(cgen->output, "%s %s", c_type_for(cgen, param_type),
                            param->param.sym->mangled_name);
                } else if (param->param.is_mut_recv) {
                    fprintf(cgen->output, "%s *%s", c_type_for(cgen, param_type),
                            param->param.sym->mangled_name);
                } else {
                    fprintf(cgen->output, "const %s *%s", c_type_for(cgen, param_type),
                            param->param.sym->mangled_name);
                }
            } else {
                fprintf(cgen->output, "%s %s", c_type_for(cgen, param_type),
                        param->param.sym->mangled_name);
            }
        }
    }
}

static void emit_fn_decl(CGen *cgen, const HirNode *node, bool forward_only) {
    emit_fn_sig(cgen, node);

    if (forward_only) {
        fprintf(cgen->output, ");\n");
        return;
    }

    fprintf(cgen->output, ") {\n");
    cgen->indent++;
    emit_fn_body(cgen, node);
    cgen->indent--;
    emit_line(cgen, "}");
    fprintf(cgen->output, "\n");
}

// ── Preamble + file emission ───────────────────────────────────────────

/** Emit the preamble: generated-file warning and required C headers. */
static void emit_preamble(CGen *cgen) {
    emit(cgen, "// Generated by resurg pipeline - do not edit.\n");
    emit(cgen, "#include <stdint.h>\n");
    emit(cgen, "#include <stdbool.h>\n");
    emit(cgen, "#include \"%s\"\n\n", cgen->abi->runtime_header);
}

/** Return true if @p node is a top-level stmt. */
static bool is_top_level_stmt(const HirNode *node) {
    return node->kind != HIR_MODULE && node->kind != HIR_FN_DECL && node->kind != HIR_TYPE_ALIAS &&
           node->kind != HIR_STRUCT_DECL && node->kind != HIR_ENUM_DECL;
}

/**
 * Emit the auto-generated `main()` entry point that calls
 * `_rsg_top_level()`, the user's `main()`, and all zero-param
 * `test_*` fns.
 */
static void emit_entry_point(CGen *cgen, const HirNode *file, bool has_top_stmts) {
    emit(cgen, "int main(void) {\n");
    cgen->indent++;

    // Initialise the tracing GC with the stack bottom anchor.
    emit_line(cgen, "int " RSG_INTERNAL_GC_ANCHOR ";");
    emit_line(cgen, "%s(&" RSG_INTERNAL_GC_ANCHOR ");", cgen->abi->gc_init);

    if (has_top_stmts) {
        emit_line(cgen, RSG_INTERNAL_TOP_LEVEL "();");
    }

    for (int32_t i = 0; i < BUF_LEN(file->file.decls); i++) {
        const HirNode *decl = file->file.decls[i];
        if (decl->kind != HIR_FN_DECL) {
            continue;
        }
        const char *name = decl->fn_decl.name;
        bool is_main = strcmp(name, "main") == 0;
        bool is_test = strncmp(name, "test_", 5) == 0;
        bool has_params = BUF_LEN(decl->fn_decl.params) != 0;
        if ((is_main || is_test) && !has_params) {
            emit_line(cgen, "%s();", decl->fn_decl.sym->mangled_name);
        }
    }

    emit_line(cgen, "return 0;");
    cgen->indent--;
    emit(cgen, "}\n");
}

/** Emit module comment and register compound types. */
static void emit_module_and_types(CGen *cgen, const HirNode *file) {
    for (int32_t i = 0; i < BUF_LEN(file->file.decls); i++) {
        const HirNode *decl = file->file.decls[i];
        if (decl->kind == HIR_MODULE) {
            cgen->module = decl->module.name;
            emit(cgen, "// module %s\n\n", decl->module.name);
        }
    }

    clear_compound_types(cgen);
    for (int32_t i = 0; i < BUF_LEN(file->file.compound_types); i++) {
        BUF_PUSH(cgen->compound_types, file->file.compound_types[i]);
    }
    emit_compound_typedefs(cgen);
}

/** Emit forward decls, then full defs, for all fns. */
static void emit_fns(CGen *cgen, const HirNode *file) {
    // Forward declarations
    for (int32_t i = 0; i < BUF_LEN(file->file.decls); i++) {
        const HirNode *decl = file->file.decls[i];
        if (decl->kind == HIR_FN_DECL) {
            emit_fn_decl(cgen, decl, true);
        }
    }
    emit(cgen, "\n");

    // Emit function bodies to a temp file so companion functions
    // (wrappers, closures) are written to real_output first.
    FILE *real_out = cgen->output;
    cgen->real_output = real_out;
    FILE *body_tmp = tmpfile();
    if (body_tmp == NULL) {
        rsg_fatal("emit_fns: failed to create temporary file");
    }
    cgen->output = body_tmp;

    for (int32_t i = 0; i < BUF_LEN(file->file.decls); i++) {
        const HirNode *decl = file->file.decls[i];
        if (decl->kind == HIR_FN_DECL) {
            emit_fn_decl(cgen, decl, false);
        }
    }

    // Flush: write companion functions (on real_out), then body buffer
    FILE *saved_body = cgen->output;
    cgen->output = real_out;
    cgen->real_output = NULL;

    fseek(saved_body, 0, SEEK_SET);
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), saved_body)) > 0) {
        fwrite(buf, 1, n, real_out);
    }
    fclose(saved_body);
}

/** Emit `_rsg_top_level()` wrapper if any top-level stmts exist. */
static bool emit_top_level_wrapper(CGen *cgen, const HirNode *file) {
    bool has_top_stmts = false;
    for (int32_t i = 0; i < BUF_LEN(file->file.decls); i++) {
        if (is_top_level_stmt(file->file.decls[i])) {
            has_top_stmts = true;
            break;
        }
    }
    if (!has_top_stmts) {
        return false;
    }

    emit(cgen, "static void " RSG_INTERNAL_TOP_LEVEL "(void) {\n");
    cgen->indent++;
    for (int32_t i = 0; i < BUF_LEN(file->file.decls); i++) {
        if (is_top_level_stmt(file->file.decls[i])) {
            emit_stmt(cgen, file->file.decls[i]);
        }
    }
    cgen->indent--;
    emit(cgen, "}\n\n");
    return true;
}

/** Emit file-scope declarations for pub immut global vars (no initializer). */
static void emit_global_consts(CGen *cgen, const HirNode *file) {
    bool any = false;
    for (int32_t i = 0; i < BUF_LEN(file->file.decls); i++) {
        const HirNode *decl = file->file.decls[i];
        if (decl->kind == HIR_VAR_DECL && decl->var_decl.is_global) {
            const Type *type = decl->var_decl.var_type;
            const char *c_name = decl->var_decl.sym->mangled_name;
            emit_line(cgen, "static %s %s;", c_type_for(cgen, type), c_name);
            any = true;
        }
    }
    if (any) {
        emit(cgen, "\n");
    }
}

/**
 * Emit the full C translation unit: preamble, module comment, compound
 * types, forward decls, fn defs, and (if present)
 * a `_rsg_top_level()` wrapper for top-level stmts.
 */
void emit_file(CGen *cgen, const HirNode *file) {
    emit_preamble(cgen);
    emit_module_and_types(cgen, file);
    emit_global_consts(cgen, file);
    emit_fns(cgen, file);
    bool has_top_stmts = emit_top_level_wrapper(cgen, file);
    emit_entry_point(cgen, file, has_top_stmts);
}
