#include "_lower.h"

// ── Match / pattern lower ──────────────────────────────────────────

/** Lower a match arm cond from the AST pattern. */
HirNode *lower_pattern_cond(Lower *low, const ASTPattern *pattern, const PatternOperand *operand,
                            SrcLoc loc) {
    switch (pattern->kind) {
    case PATTERN_WILDCARD:
    case PATTERN_BINDING:
        return NULL; // always matches

    case PATTERN_LIT: {
        // Unit literal always matches — only one value of type unit
        if (pattern->lit->lit.kind == LIT_UNIT) {
            return NULL;
        }
        HirNode *lit = lower_expr(low, pattern->lit);
        // Str comparison via rsg_str_equal
        if (operand->type != NULL && operand->type->kind == TYPE_STR) {
            HirNode **args = NULL;
            BUF_PUSH(args, operand->ref);
            BUF_PUSH(args, lit);
            return lower_make_builtin_call(
                low,
                &(BuiltinCallSpec){RSG_FN_STR_EQUAL, &TYPE_BOOL_INST, args, loc, INTRINSIC_NONE});
        }
        HirNode *cmp = hir_new(low->hir_arena, HIR_BINARY, &TYPE_BOOL_INST, loc);
        cmp->binary.op = TOKEN_EQUAL_EQUAL;
        cmp->binary.left = operand->ref;
        cmp->binary.right = lit;
        return cmp;
    }

    case PATTERN_RANGE: {
        HirNode *start = lower_expr(low, pattern->range_start);
        HirNode *end = lower_expr(low, pattern->range_end);

        HirNode *ge = hir_new(low->hir_arena, HIR_BINARY, &TYPE_BOOL_INST, loc);
        ge->binary.op = TOKEN_GREATER_EQUAL;
        ge->binary.left = operand->ref;
        ge->binary.right = start;

        HirNode *upper = hir_new(low->hir_arena, HIR_BINARY, &TYPE_BOOL_INST, loc);
        upper->binary.op = pattern->range_inclusive ? TOKEN_LESS_EQUAL : TOKEN_LESS;
        upper->binary.left = operand->ref;
        upper->binary.right = end;

        HirNode *combined = hir_new(low->hir_arena, HIR_BINARY, &TYPE_BOOL_INST, loc);
        combined->binary.op = TOKEN_AMPERSAND_AMPERSAND;
        combined->binary.left = ge;
        combined->binary.right = upper;
        return combined;
    }

    case PATTERN_VARIANT_UNIT:
    case PATTERN_VARIANT_TUPLE:
    case PATTERN_VARIANT_STRUCT: {
        const EnumVariant *variant = type_enum_find_variant(operand->type, pattern->name);
        if (variant == NULL) {
            return NULL;
        }
        HirNode *tag_access = lower_make_field_access(
            low, &(FieldAccessSpec){operand->ref, "_tag", &TYPE_I32_INST, false, loc});

        HirNode *tag_val = lower_make_int_lit(
            low, &(IntLitSpec){(uint64_t)variant->discriminant, &TYPE_I32_INST, TYPE_I32, loc});
        HirNode *cmp = hir_new(low->hir_arena, HIR_BINARY, &TYPE_BOOL_INST, loc);
        cmp->binary.op = TOKEN_EQUAL_EQUAL;
        cmp->binary.left = tag_access;
        cmp->binary.right = tag_val;
        return cmp;
    }
    }
    return NULL;
}

/** Register match arm bindings: extract vars from variant patterns. */
void lower_pattern_bindings(Lower *low, const ASTPattern *pattern, const Type *operand_type) {
    if (pattern->kind == PATTERN_BINDING && pattern->name != NULL) {
        HirSym *var_sym = lower_add_var(
            low, &(HirSymSpec){HIR_SYM_VAR, pattern->name, operand_type, false, pattern->loc});
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
                lower_add_var(low, &(HirSymSpec){HIR_SYM_VAR, sub->name, variant->tuple_types[i],
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
                    lower_add_var(low, &(HirSymSpec){HIR_SYM_VAR, fname, variant->fields[j].type,
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
HirNode *lower_arm_bindings_block(Lower *low, const ASTPattern *pattern,
                                  const PatternOperand *operand, SrcLoc loc) {
    HirNode **bind_stmts = NULL;

    switch (pattern->kind) {
    case PATTERN_BINDING: {
        HirSym *bsym = lower_scope_lookup(low, pattern->name);
        if (bsym != NULL) {
            HirNode *init = lower_make_var_ref(low, operand->sym, loc);
            BUF_PUSH(bind_stmts, lower_make_var_decl(low, bsym, init));
        }
        break;
    }
    case PATTERN_VARIANT_TUPLE: {
        const EnumVariant *variant = type_enum_find_variant(operand->type, pattern->name);
        if (variant == NULL) {
            break;
        }
        for (int32_t j = 0; j < BUF_LEN(pattern->sub_patterns); j++) {
            ASTPattern *sub = pattern->sub_patterns[j];
            if (sub->kind != PATTERN_BINDING || sub->name == NULL || j >= variant->tuple_count) {
                continue;
            }
            // Skip unit-typed tuple fields (no data in C union for void)
            if (variant->tuple_types[j]->kind == TYPE_UNIT) {
                continue;
            }
            HirSym *bsym = lower_scope_lookup(low, sub->name);
            if (bsym == NULL) {
                continue;
            }
            HirNode *data_access = lower_make_field_access(
                low,
                &(FieldAccessSpec){lower_make_var_ref(low, operand->sym, loc),
                                   arena_sprintf(low->hir_arena, "_data.%s._%d", pattern->name, j),
                                   variant->tuple_types[j], false, loc});
            BUF_PUSH(bind_stmts, lower_make_var_decl(low, bsym, data_access));
        }
        break;
    }
    case PATTERN_VARIANT_STRUCT: {
        const EnumVariant *variant = type_enum_find_variant(operand->type, pattern->name);
        if (variant == NULL) {
            break;
        }
        for (int32_t j = 0; j < BUF_LEN(pattern->field_names); j++) {
            const char *fname = pattern->field_names[j];
            HirSym *bsym = lower_scope_lookup(low, fname);
            if (bsym == NULL) {
                continue;
            }
            const Type *ftype = hir_sym_type(bsym);
            HirNode *data_access = lower_make_field_access(
                low, &(FieldAccessSpec){
                         lower_make_var_ref(low, operand->sym, loc),
                         arena_sprintf(low->hir_arena, "_data.%s.%s", pattern->name, fname), ftype,
                         false, loc});
            BUF_PUSH(bind_stmts, lower_make_var_decl(low, bsym, data_access));
        }
        break;
    }
    default:
        return NULL;
    }

    if (BUF_LEN(bind_stmts) == 0) {
        return NULL;
    }
    HirNode *block = hir_new(low->hir_arena, HIR_BLOCK, &TYPE_UNIT_INST, loc);
    block->block.stmts = bind_stmts;
    block->block.result = NULL;
    return block;
}

HirNode *lower_match(Lower *low, const ASTNode *ast) {
    HirNode *operand = lower_expr(low, ast->match_expr.operand);
    const Type *operand_type = operand->type;
    SrcLoc loc = ast->loc;

    // Auto-deref: insert deref for *T
    if (operand_type != NULL && operand_type->kind == TYPE_PTR) {
        const Type *pointee = operand_type->ptr.pointee;
        HirNode *deref = hir_new(low->hir_arena, HIR_DEREF, pointee, loc);
        deref->deref.operand = operand;
        operand = deref;
        operand_type = pointee;
    }

    // Store operand in a temp to avoid re-evaluation
    const char *match_tmp = lower_make_temp_name(low);
    HirSymSpec match_spec = {HIR_SYM_VAR, match_tmp, operand_type, false, loc};
    HirSym *match_sym = lower_add_var(low, &match_spec);

    HirMatchArm *arms = NULL;

    for (int32_t i = 0; i < BUF_LEN(ast->match_expr.arms); i++) {
        const ASTMatchArm *arm = &ast->match_expr.arms[i];

        HirNode *operand_ref = lower_make_var_ref(low, match_sym, loc);
        PatternOperand operand = {operand_ref, match_sym, operand_type};

        HirNode *cond = lower_pattern_cond(low, arm->pattern, &operand, arm->pattern->loc);

        // Guard — bindings must be emitted before guard in codegen
        HirNode *guard = NULL;
        if (arm->guard != NULL) {
            lower_scope_enter(low);
            lower_pattern_bindings(low, arm->pattern, operand_type);
            guard = lower_expr(low, arm->guard);
            lower_scope_leave(low);
        }

        lower_scope_enter(low);
        lower_pattern_bindings(low, arm->pattern, operand_type);

        HirNode *body = lower_expr(low, arm->body);

        HirMatchArm hir_arm = {
            .cond = cond,
            .guard = guard,
            .body = body,
            .bindings = lower_arm_bindings_block(low, arm->pattern, &operand, loc),
        };
        BUF_PUSH(arms, hir_arm);
        lower_scope_leave(low);
    }

    HirNode *match_node = hir_new(low->hir_arena, HIR_MATCH, ast->type, loc);
    match_node->match_expr.operand = lower_make_var_decl(low, match_sym, operand);
    match_node->match_expr.arms = arms;
    return match_node;
}
