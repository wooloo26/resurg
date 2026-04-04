#include "_lowering.h"

// ── String interpolation lowering ──────────────────────────────────────

/** Return the rsg_string_from_* function name for @p type, or NULL. */
static const char *string_conversion_builtin(const Type *type) {
    if (type == NULL) {
        return NULL;
    }
    static const struct {
        TypeKind kind;
        const char *name;
    } conversions[] = {
        {TYPE_I32, "rsg_string_from_i32"},   {TYPE_U32, "rsg_string_from_u32"},
        {TYPE_I64, "rsg_string_from_i64"},   {TYPE_U64, "rsg_string_from_u64"},
        {TYPE_F32, "rsg_string_from_f32"},   {TYPE_F64, "rsg_string_from_f64"},
        {TYPE_BOOL, "rsg_string_from_bool"}, {TYPE_CHAR, "rsg_string_from_char"},
    };
    for (size_t i = 0; i < sizeof(conversions) / sizeof(conversions[0]); i++) {
        if (type->kind == conversions[i].kind) {
            return conversions[i].name;
        }
    }
    return NULL;
}

/** Lower a string part to a TYPE_STRING TtNode. */
static TtNode *lower_string_part(Lowering *low, const ASTNode *part) {
    TtNode *lowered = lower_expression(low, part);
    if (lowered->type != NULL && lowered->type->kind == TYPE_STRING) {
        return lowered;
    }
    // Wrap in rsg_string_from_TYPE call
    const char *builtin = string_conversion_builtin(lowered->type);
    if (builtin != NULL) {
        TtNode **args = NULL;
        BUFFER_PUSH(args, lowered);
        return lowering_make_builtin_call(low, builtin, &TYPE_STRING_INSTANCE, args,
                                          part->location);
    }
    return lowered;
}

/** Collect non-empty string parts into a buffer. Returns NULL if empty. */
static TtNode **collect_string_parts(Lowering *low, const ASTNode *ast) {
    int32_t part_count = BUFFER_LENGTH(ast->string_interpolation.parts);
    TtNode **string_parts = NULL;
    for (int32_t i = 0; i < part_count; i++) {
        const ASTNode *part = ast->string_interpolation.parts[i];
        if (part->kind == NODE_LITERAL && part->literal.kind == LITERAL_STRING) {
            if (part->literal.string_value == NULL || part->literal.string_value[0] == '\0') {
                continue;
            }
        }
        BUFFER_PUSH(string_parts, lower_string_part(low, part));
    }
    return string_parts;
}

/** Chain string parts with rsg_string_concat: concat(concat(a, b), c). */
static TtNode *chain_string_concat(Lowering *low, TtNode **parts, int32_t count,
                                   SourceLocation loc) {
    TtNode *result = parts[0];
    for (int32_t i = 1; i < count; i++) {
        TtNode **args = NULL;
        BUFFER_PUSH(args, result);
        BUFFER_PUSH(args, parts[i]);
        result =
            lowering_make_builtin_call(low, "rsg_string_concat", &TYPE_STRING_INSTANCE, args, loc);
    }
    return result;
}

TtNode *lower_string_interpolation(Lowering *low, const ASTNode *ast) {
    int32_t part_count = BUFFER_LENGTH(ast->string_interpolation.parts);
    if (part_count == 0) {
        TtNode *node =
            tt_new(low->tt_arena, TT_STRING_LITERAL, &TYPE_STRING_INSTANCE, ast->location);
        node->string_literal.value = "";
        return node;
    }

    TtNode **string_parts = collect_string_parts(low, ast);
    int32_t count = BUFFER_LENGTH(string_parts);

    if (count == 0) {
        TtNode *node =
            tt_new(low->tt_arena, TT_STRING_LITERAL, &TYPE_STRING_INSTANCE, ast->location);
        node->string_literal.value = "";
        BUFFER_FREE(string_parts);
        return node;
    }
    if (count == 1) {
        TtNode *result = string_parts[0];
        BUFFER_FREE(string_parts);
        return result;
    }

    TtNode *result = chain_string_concat(low, string_parts, count, ast->location);
    BUFFER_FREE(string_parts);
    return result;
}
