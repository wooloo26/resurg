#include "_lower.h"

// ── Str interpolation lower ──────────────────────────────────────

/** Lower a str part to a TYPE_STR HirNode. */
static HirNode *lower_str_part(Lower *low, const ASTNode *part) {
    HirNode *lowered = lower_expr(low, part);
    if (lowered->type != NULL && lowered->type->kind == TYPE_STR) {
        return lowered;
    }
    // Wrap in rsg_str_from_TYPE call (type must have TF_STR_CONV flag)
    const char *suffix = type_runtime_suffix(lowered->type);
    if (suffix != NULL && type_is_str_convertible(lowered->type)) {
        const char *fn = arena_sprintf(low->hir_arena, RSG_FN_STR_FROM "%s", suffix);
        HirNode **args = NULL;
        BUF_PUSH(args, lowered);
        return lower_make_builtin_call(
            low, &(BuiltinCallSpec){fn, &TYPE_STR_INST, args, part->loc, INTRINSIC_NONE});
    }
    return lowered;
}

/** Collect non-empty str parts into a buf, skipping empty literals. */
static HirNode **collect_nonempty_str_parts(Lower *low, const ASTNode *ast) {
    int32_t part_count = BUF_LEN(ast->str_interpolation.parts);
    HirNode **str_parts = NULL;
    for (int32_t i = 0; i < part_count; i++) {
        const ASTNode *part = ast->str_interpolation.parts[i];
        if (part->kind == NODE_LIT && part->lit.kind == LIT_STR) {
            if (part->lit.str_value == NULL || part->lit.str_value[0] == '\0') {
                continue;
            }
        }
        BUF_PUSH(str_parts, lower_str_part(low, part));
    }
    return str_parts;
}

/** Chain str parts with rsg_str_concat: concat(concat(a, b), c). */
static HirNode *chain_str_concat(Lower *low, HirNode **parts, int32_t count, SrcLoc loc) {
    HirNode *result = parts[0];
    for (int32_t i = 1; i < count; i++) {
        HirNode **args = NULL;
        BUF_PUSH(args, result);
        BUF_PUSH(args, parts[i]);
        result = lower_make_builtin_call(
            low, &(BuiltinCallSpec){RSG_FN_STR_CONCAT, &TYPE_STR_INST, args, loc, INTRINSIC_NONE});
    }
    return result;
}

HirNode *lower_str_interpolation(Lower *low, const ASTNode *ast) {
    int32_t part_count = BUF_LEN(ast->str_interpolation.parts);
    if (part_count == 0) {
        HirNode *node = hir_new(low->hir_arena, HIR_STR_LIT, &TYPE_STR_INST, ast->loc);
        node->str_lit.value = "";
        return node;
    }

    HirNode **str_parts = collect_nonempty_str_parts(low, ast);
    int32_t count = BUF_LEN(str_parts);

    if (count == 0) {
        HirNode *node = hir_new(low->hir_arena, HIR_STR_LIT, &TYPE_STR_INST, ast->loc);
        node->str_lit.value = "";
        BUF_FREE(str_parts);
        return node;
    }
    if (count == 1) {
        HirNode *result = str_parts[0];
        BUF_FREE(str_parts);
        return result;
    }

    HirNode *result = chain_str_concat(low, str_parts, count, ast->loc);
    BUF_FREE(str_parts);
    return result;
}
