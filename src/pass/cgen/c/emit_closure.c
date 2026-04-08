#include "helpers.h"

// ── Closure + function-reference emission ──────────────────────────

/**
 * Emit a static wrapper fn that adapts a plain fn to the RsgFn convention
 * (first param is unused void *_env).  Returns the wrapper name.
 * Deduplicates via cgen->wrapper_set.
 */
static const char *emit_fn_ref_wrapper(CGen *cgen, const HirSym *fn_sym, const Type *fn_type) {
    const char *mangled = fn_sym->mangled_name;
    const char *wrapper_name = arena_sprintf(cgen->arena, "_rsg_wrap_%s", mangled);

    // Dedup: skip if already emitted
    if (hash_table_lookup(&cgen->wrapper_set, wrapper_name) != NULL) {
        return wrapper_name;
    }
    hash_table_insert(&cgen->wrapper_set, wrapper_name, (void *)wrapper_name);

    if (fn_type == NULL || fn_type->kind != TYPE_FN) {
        return wrapper_name;
    }

    // Switch to real output for companion emission
    FILE *saved = cgen->output;
    if (cgen->real_output != NULL) {
        cgen->output = cgen->real_output;
    }
    int32_t saved_indent = cgen->indent;
    cgen->indent = 0;

    // Emit: static RetType wrapper(void *_env, P1 p1, P2 p2, ...) {
    //           (void)_env; return mangled(p1, p2, ...);
    //       }
    const char *ret_type = c_type_for(cgen, fn_type->fn_type.return_type);
    emit(cgen, "static %s %s(void *_env", ret_type, wrapper_name);
    for (int32_t i = 0; i < fn_type->fn_type.param_count; i++) {
        const char *pt = c_type_for(cgen, fn_type->fn_type.params[i]);
        emit(cgen, ", %s _p%d", pt, i);
    }
    emit(cgen, ") {\n");
    emit(cgen, "    (void)_env;\n");
    if (fn_type->fn_type.return_type->kind != TYPE_UNIT) {
        emit(cgen, "    return %s(", mangled);
    } else {
        emit(cgen, "    %s(", mangled);
    }
    for (int32_t i = 0; i < fn_type->fn_type.param_count; i++) {
        if (i > 0) {
            emit(cgen, ", ");
        }
        emit(cgen, "_p%d", i);
    }
    emit(cgen, ");\n}\n\n");

    cgen->output = saved;
    cgen->indent = saved_indent;
    return wrapper_name;
}

const char *emit_fn_ref_expr(CGen *cgen, const HirNode *node) {
    const char *wrapper_name = emit_fn_ref_wrapper(cgen, node->var_ref.sym, node->type);
    return arena_sprintf(cgen->arena, "(RsgFn){(void(*)(void))%s, NULL}", wrapper_name);
}

/** Emit the typedef for a closure's captured-variable env struct. */
static const char *emit_closure_env_struct(CGen *cgen, const HirNode *node) {
    const char *env_struct = arena_sprintf(cgen->arena, "%s_env", node->closure.fn_name);
    emit(cgen, "typedef struct {\n");
    for (int32_t i = 0; i < BUF_LEN(node->closure.capture_syms); i++) {
        const char *ct = c_type_for(cgen, node->closure.capture_syms[i]->type);
        emit(cgen, "    %s %s;\n", ct, node->closure.capture_names[i]);
    }
    emit(cgen, "} %s;\n\n", env_struct);
    return env_struct;
}

/** Unpack captures into the closure body: #define macros (FnMut) or local copies (Fn). */
static void emit_closure_capture_unpack(CGen *cgen, const HirNode *node, const char *env_struct) {
    int32_t num_captures = BUF_LEN(node->closure.capture_syms);
    if (num_captures == 0) {
        emit_line(cgen, "(void)_env;");
        return;
    }

    emit_line(cgen, "%s *_cap = (%s *)_env;", env_struct, env_struct);
    if (node->closure.is_fn_mut) {
        for (int32_t i = 0; i < num_captures; i++) {
            emit(cgen, "#define %s _cap->%s\n", node->closure.capture_names[i],
                 node->closure.capture_names[i]);
        }
    } else {
        for (int32_t i = 0; i < num_captures; i++) {
            const char *ct = c_type_for(cgen, node->closure.capture_syms[i]->type);
            emit_line(cgen, "%s %s = _cap->%s;", ct, node->closure.capture_names[i],
                      node->closure.capture_names[i]);
        }
    }
}

/** Emit companion function + env struct for a closure. */
static void emit_closure_companion(CGen *cgen, const HirNode *node) {
    const char *fn_name = node->closure.fn_name;
    int32_t num_captures = BUF_LEN(node->closure.capture_syms);
    const Type *fn_type = node->type;

    // Switch to real output
    FILE *saved = cgen->output;
    if (cgen->real_output != NULL) {
        cgen->output = cgen->real_output;
    }
    int32_t saved_indent = cgen->indent;
    cgen->indent = 0;

    // Emit env struct typedef (if captures)
    const char *env_struct = NULL;
    if (num_captures > 0) {
        env_struct = emit_closure_env_struct(cgen, node);
    }

    // Emit closure function: static RetType fn_name(void *_env, P1 p1, ...) { body }
    const char *ret_type = c_type_for(cgen, fn_type->fn_type.return_type);
    emit(cgen, "static %s %s(void *_env", ret_type, fn_name);
    for (int32_t i = 0; i < BUF_LEN(node->closure.params); i++) {
        const char *pt = c_type_for(cgen, node->closure.params[i]->type);
        const char *pn = node->closure.params[i]->param.sym->mangled_name;
        emit(cgen, ", %s %s", pt, pn);
    }
    emit(cgen, ") {\n");
    cgen->indent = 1;

    emit_closure_capture_unpack(cgen, node, env_struct);

    // Emit body
    const HirNode *body = node->closure.body;
    if (body->kind == HIR_BLOCK) {
        emit_block_stmts(cgen, body);
        if (body->block.result != NULL) {
            const char *result = emit_expr(cgen, body->block.result);
            emit_line(cgen, "return %s;", result);
        }
    } else {
        const char *result = emit_expr(cgen, body);
        if (fn_type->fn_type.return_type->kind != TYPE_UNIT) {
            emit_line(cgen, "return %s;", result);
        } else {
            emit_line(cgen, "%s;", result);
        }
    }

    cgen->indent = 0;
    // Undef FnMut capture macros
    if (node->closure.is_fn_mut && num_captures > 0) {
        for (int32_t i = 0; i < num_captures; i++) {
            emit(cgen, "#undef %s\n", node->closure.capture_names[i]);
        }
    }
    emit(cgen, "}\n\n");

    cgen->output = saved;
    cgen->indent = saved_indent;
}

const char *emit_closure_expr(CGen *cgen, const HirNode *node) {
    emit_closure_companion(cgen, node);

    const char *fn_name = node->closure.fn_name;
    int32_t num_captures = BUF_LEN(node->closure.capture_syms);

    if (num_captures == 0) {
        return arena_sprintf(cgen->arena, "(RsgFn){(void(*)(void))%s, NULL}", fn_name);
    }

    // Capturing closure: emit env allocation + construction
    const char *env_struct = arena_sprintf(cgen->arena, "%s_env", fn_name);
    const char *env_tmp = next_temp(cgen);
    emit_line(cgen, "%s *%s = rsg_heap_alloc(sizeof(%s));", env_struct, env_tmp, env_struct);
    for (int32_t i = 0; i < num_captures; i++) {
        emit_line(cgen, "%s->%s = %s;", env_tmp, node->closure.capture_names[i],
                  node->closure.capture_syms[i]->mangled_name);
    }
    const char *result = next_temp(cgen);
    emit_line(cgen, "RsgFn %s = (RsgFn){(void(*)(void))%s, (void *)%s};", result, fn_name, env_tmp);
    return result;
}
