#include "_lowering.h"

// ── Match / pattern lowering ──────────────────────────────────────────

/** Lower a match arm cond from the AST pattern. */
static TTNode *lower_pattern_cond(Lowering *low, const ASTPattern *pattern, TTNode *operand_ref,
                                  const Type *operand_type, SourceLoc loc) {
    switch (pattern->kind) {
    case PATTERN_WILDCARD:
    case PATTERN_BINDING:
        return NULL; // always matches

    case PATTERN_LIT: {
        TTNode *lit = lower_expr(low, pattern->lit);
        // Str comparison via rsg_str_equal
        if (operand_type != NULL && operand_type->kind == TYPE_STR) {
            TTNode **args = NULL;
            BUF_PUSH(args, operand_ref);
            BUF_PUSH(args, lit);
            return lowering_make_builtin_call(
                low, &(BuiltinCallSpec){"rsg_str_equal", &TYPE_BOOL_INST, args, loc});
        }
        TTNode *cmp = tt_new(low->tt_arena, TT_BINARY, &TYPE_BOOL_INST, loc);
        cmp->binary.op = TOKEN_EQUAL_EQUAL;
        cmp->binary.left = operand_ref;
        cmp->binary.right = lit;
        return cmp;
    }

    case PATTERN_RANGE: {
        TTNode *start = lower_expr(low, pattern->range_start);
        TTNode *end = lower_expr(low, pattern->range_end);

        TTNode *ge = tt_new(low->tt_arena, TT_BINARY, &TYPE_BOOL_INST, loc);
        ge->binary.op = TOKEN_GREATER_EQUAL;
        ge->binary.left = operand_ref;
        ge->binary.right = start;

        TTNode *upper = tt_new(low->tt_arena, TT_BINARY, &TYPE_BOOL_INST, loc);
        upper->binary.op = pattern->range_inclusive ? TOKEN_LESS_EQUAL : TOKEN_LESS;
        upper->binary.left = operand_ref;
        upper->binary.right = end;

        TTNode *combined = tt_new(low->tt_arena, TT_BINARY, &TYPE_BOOL_INST, loc);
        combined->binary.op = TOKEN_AMPERSAND_AMPERSAND;
        combined->binary.left = ge;
        combined->binary.right = upper;
        return combined;
    }

    case PATTERN_VARIANT_UNIT:
    case PATTERN_VARIANT_TUPLE:
    case PATTERN_VARIANT_STRUCT: {
        const EnumVariant *variant = type_enum_find_variant(operand_type, pattern->name);
        if (variant == NULL) {
            return NULL;
        }
        TTNode *tag_access = tt_new(low->tt_arena, TT_STRUCT_FIELD_ACCESS, &TYPE_I32_INST, loc);
        tag_access->struct_field_access.object = operand_ref;
        tag_access->struct_field_access.field = "_tag";
        tag_access->struct_field_access.via_ptr = false;

        TTNode *tag_val = lowering_make_int_lit(
            low, &(IntLitSpec){(uint64_t)variant->discriminant, &TYPE_I32_INST, TYPE_I32, loc});
        TTNode *cmp = tt_new(low->tt_arena, TT_BINARY, &TYPE_BOOL_INST, loc);
        cmp->binary.op = TOKEN_EQUAL_EQUAL;
        cmp->binary.left = tag_access;
        cmp->binary.right = tag_val;
        return cmp;
    }
    }
    return NULL;
}

/** Register match arm bindings: extract vars from variant patterns. */
static void lower_pattern_bindings(Lowering *low, const ASTPattern *pattern, TTSym *operand_sym,
                                   const Type *operand_type) {
    (void)operand_sym;
    if (pattern->kind == PATTERN_BINDING && pattern->name != NULL) {
        TTSym *var_sym = lowering_add_var(
            low, &(TtSymSpec){TT_SYM_VAR, pattern->name, operand_type, false, pattern->loc});
        (void)var_sym;
        return;
    }

    if (pattern->kind == PATTERN_VARIANT_TUPLE && operand_type != NULL &&
        operand_type->kind == TYPE_ENUM) {
        const EnumVariant *variant = type_enum_find_variant(operand_type, pattern->name);
        if (variant == NULL) {
            return;
        }
        for (int32_t i = 0; i < BUF_LEN(pattern->sub_patterns); i++) {
            ASTPattern *sub = pattern->sub_patterns[i];
            if (sub->kind == PATTERN_BINDING && sub->name != NULL && i < variant->tuple_count) {
                lowering_add_var(low, &(TtSymSpec){TT_SYM_VAR, sub->name, variant->tuple_types[i],
                                                   false, sub->loc});
            }
        }
    }

    if (pattern->kind == PATTERN_VARIANT_STRUCT && operand_type != NULL &&
        operand_type->kind == TYPE_ENUM) {
        const EnumVariant *variant = type_enum_find_variant(operand_type, pattern->name);
        if (variant == NULL) {
            return;
        }
        for (int32_t i = 0; i < BUF_LEN(pattern->field_names); i++) {
            const char *fname = pattern->field_names[i];
            for (int32_t j = 0; j < variant->field_count; j++) {
                if (strcmp(variant->fields[j].name, fname) == 0) {
                    lowering_add_var(low, &(TtSymSpec){TT_SYM_VAR, fname, variant->fields[j].type,
                                                       false, pattern->loc});
                    break;
                }
            }
        }
    }
}

/**
 * Build the bindings block for a single match arm.
 *
 * Extracts vars from the pattern and initializes them from the
 * match operand.  Returns NULL when no bindings are needed.
 */
static TTNode *lower_arm_bindings_block(Lowering *low, const ASTPattern *pattern,
                                        TTSym *operand_sym, const Type *operand_type,
                                        SourceLoc loc) {
    TTNode **bind_stmts = NULL;

    switch (pattern->kind) {
    case PATTERN_BINDING: {
        TTSym *bsym = lowering_scope_find(low, pattern->name);
        if (bsym != NULL) {
            TTNode *init = lowering_make_var_ref(low, operand_sym, loc);
            BUF_PUSH(bind_stmts, lowering_make_var_decl(low, bsym, init));
        }
        break;
    }
    case PATTERN_VARIANT_TUPLE: {
        const EnumVariant *variant = type_enum_find_variant(operand_type, pattern->name);
        if (variant == NULL) {
            break;
        }
        for (int32_t j = 0; j < BUF_LEN(pattern->sub_patterns); j++) {
            ASTPattern *sub = pattern->sub_patterns[j];
            if (sub->kind != PATTERN_BINDING || sub->name == NULL || j >= variant->tuple_count) {
                continue;
            }
            TTSym *bsym = lowering_scope_find(low, sub->name);
            if (bsym == NULL) {
                continue;
            }
            TTNode *data_access =
                tt_new(low->tt_arena, TT_STRUCT_FIELD_ACCESS, variant->tuple_types[j], loc);
            data_access->struct_field_access.object = lowering_make_var_ref(low, operand_sym, loc);
            data_access->struct_field_access.field =
                arena_sprintf(low->tt_arena, "_data.%s._%d", pattern->name, j);
            data_access->struct_field_access.via_ptr = false;
            BUF_PUSH(bind_stmts, lowering_make_var_decl(low, bsym, data_access));
        }
        break;
    }
    case PATTERN_VARIANT_STRUCT: {
        const EnumVariant *variant = type_enum_find_variant(operand_type, pattern->name);
        if (variant == NULL) {
            break;
        }
        for (int32_t j = 0; j < BUF_LEN(pattern->field_names); j++) {
            const char *fname = pattern->field_names[j];
            TTSym *bsym = lowering_scope_find(low, fname);
            if (bsym == NULL) {
                continue;
            }
            const Type *ftype = tt_sym_type(bsym);
            TTNode *data_access = tt_new(low->tt_arena, TT_STRUCT_FIELD_ACCESS, ftype, loc);
            data_access->struct_field_access.object = lowering_make_var_ref(low, operand_sym, loc);
            data_access->struct_field_access.field =
                arena_sprintf(low->tt_arena, "_data.%s.%s", pattern->name, fname);
            data_access->struct_field_access.via_ptr = false;
            BUF_PUSH(bind_stmts, lowering_make_var_decl(low, bsym, data_access));
        }
        break;
    }
    default:
        return NULL;
    }

    if (BUF_LEN(bind_stmts) == 0) {
        return NULL;
    }
    TTNode *block = tt_new(low->tt_arena, TT_BLOCK, &TYPE_UNIT_INST, loc);
    block->block.stmts = bind_stmts;
    block->block.result = NULL;
    return block;
}

TTNode *lower_match(Lowering *low, const ASTNode *ast) {
    TTNode *operand = lower_expr(low, ast->match_expr.operand);
    const Type *operand_type = operand->type;
    SourceLoc loc = ast->loc;

    // Store operand in a temp to avoid re-evaluation
    const char *match_tmp = lowering_make_temp_name(low);
    TtSymSpec match_spec = {TT_SYM_VAR, match_tmp, operand_type, false, loc};
    TTSym *match_sym = lowering_add_var(low, &match_spec);

    TTNode **arm_conds = NULL;
    TTNode **arm_guards = NULL;
    TTNode **arm_bodies = NULL;
    TTNode **arm_bindings = NULL;

    for (int32_t i = 0; i < BUF_LEN(ast->match_expr.arms); i++) {
        const ASTMatchArm *arm = &ast->match_expr.arms[i];

        TTNode *operand_ref = lowering_make_var_ref(low, match_sym, loc);

        TTNode *cond =
            lower_pattern_cond(low, arm->pattern, operand_ref, operand_type, arm->pattern->loc);

        // Guard — bindings must be emitted before guard in codegen
        TTNode *guard = NULL;
        if (arm->guard != NULL) {
            lowering_scope_enter(low);
            lower_pattern_bindings(low, arm->pattern, match_sym, operand_type);
            guard = lower_expr(low, arm->guard);
            lowering_scope_leave(low);
        }

        BUF_PUSH(arm_conds, cond);
        BUF_PUSH(arm_guards, guard);

        lowering_scope_enter(low);
        lower_pattern_bindings(low, arm->pattern, match_sym, operand_type);

        TTNode *body = lower_expr(low, arm->body);
        BUF_PUSH(arm_bodies, body);

        BUF_PUSH(arm_bindings,
                 lower_arm_bindings_block(low, arm->pattern, match_sym, operand_type, loc));
        lowering_scope_leave(low);
    }

    TTNode *match_node = tt_new(low->tt_arena, TT_MATCH, ast->type, loc);
    match_node->match_expr.operand = lowering_make_var_decl(low, match_sym, operand);
    match_node->match_expr.arm_conds = arm_conds;
    match_node->match_expr.arm_guards = arm_guards;
    match_node->match_expr.arm_bodies = arm_bodies;
    match_node->match_expr.arm_bindings = arm_bindings;
    return match_node;
}
