#include "_lowering.h"

// ── Str interpolation lowering ──────────────────────────────────────

/** Return the rsg_str_from_* fn name for @p type, or NULL. */
static const char *lookup_str_conversion(const Type *type) {
    if (type == NULL) {
        return NULL;
    }
    static const struct {
        TypeKind kind;
        const char *name;
    } conversions[] = {
        {TYPE_I32, "rsg_str_from_i32"},   {TYPE_U32, "rsg_str_from_u32"},
        {TYPE_I64, "rsg_str_from_i64"},   {TYPE_U64, "rsg_str_from_u64"},
        {TYPE_F32, "rsg_str_from_f32"},   {TYPE_F64, "rsg_str_from_f64"},
        {TYPE_BOOL, "rsg_str_from_bool"}, {TYPE_CHAR, "rsg_str_from_char"},
    };
    for (size_t i = 0; i < sizeof(conversions) / sizeof(conversions[0]); i++) {
        if (type->kind == conversions[i].kind) {
            return conversions[i].name;
        }
    }
    return NULL;
}

/** Lower a str part to a TYPE_STR TTNode. */
static TTNode *lower_str_part(Lowering *low, const ASTNode *part) {
    TTNode *lowered = lower_expr(low, part);
    if (lowered->type != NULL && lowered->type->kind == TYPE_STR) {
        return lowered;
    }
    // Wrap in rsg_str_from_TYPE call
    const char *builtin = lookup_str_conversion(lowered->type);
    if (builtin != NULL) {
        TTNode **args = NULL;
        BUF_PUSH(args, lowered);
        return lowering_make_builtin_call(
            low, &(BuiltinCallSpec){builtin, &TYPE_STR_INST, args, part->loc});
    }
    return lowered;
}

/** Collect non-empty str parts into a buf. Returns NULL if empty. */
static TTNode **collect_str_parts(Lowering *low, const ASTNode *ast) {
    int32_t part_count = BUF_LEN(ast->str_interpolation.parts);
    TTNode **str_parts = NULL;
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
static TTNode *chain_str_concat(Lowering *low, TTNode **parts, int32_t count, SourceLoc loc) {
    TTNode *result = parts[0];
    for (int32_t i = 1; i < count; i++) {
        TTNode **args = NULL;
        BUF_PUSH(args, result);
        BUF_PUSH(args, parts[i]);
        result = lowering_make_builtin_call(
            low, &(BuiltinCallSpec){"rsg_str_concat", &TYPE_STR_INST, args, loc});
    }
    return result;
}

TTNode *lower_str_interpolation(Lowering *low, const ASTNode *ast) {
    int32_t part_count = BUF_LEN(ast->str_interpolation.parts);
    if (part_count == 0) {
        TTNode *node = tt_new(low->tt_arena, TT_STR_LIT, &TYPE_STR_INST, ast->loc);
        node->str_lit.value = "";
        return node;
    }

    TTNode **str_parts = collect_str_parts(low, ast);
    int32_t count = BUF_LEN(str_parts);

    if (count == 0) {
        TTNode *node = tt_new(low->tt_arena, TT_STR_LIT, &TYPE_STR_INST, ast->loc);
        node->str_lit.value = "";
        BUF_FREE(str_parts);
        return node;
    }
    if (count == 1) {
        TTNode *result = str_parts[0];
        BUF_FREE(str_parts);
        return result;
    }

    TTNode *result = chain_str_concat(low, str_parts, count, ast->loc);
    BUF_FREE(str_parts);
    return result;
}
