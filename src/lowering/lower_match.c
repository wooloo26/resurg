#include "_lowering.h"

// ── Match / pattern lowering ──────────────────────────────────────────

/** Lower a match arm condition from the AST pattern. */
static TtNode *lower_pattern_condition(Lowering *low, const ASTPattern *pattern,
                                       TtNode *operand_ref, const Type *operand_type,
                                       SourceLocation location) {
    switch (pattern->kind) {
    case PATTERN_WILDCARD:
    case PATTERN_BINDING:
        return NULL; // always matches

    case PATTERN_LITERAL: {
        TtNode *lit = lower_expression(low, pattern->literal);
        // String comparison via rsg_string_equal
        if (operand_type != NULL && operand_type->kind == TYPE_STRING) {
            TtNode **args = NULL;
            BUFFER_PUSH(args, operand_ref);
            BUFFER_PUSH(args, lit);
            return lowering_make_builtin_call(
                low, &(BuiltinCallSpec){"rsg_string_equal", &TYPE_BOOL_INSTANCE, args, location});
        }
        TtNode *cmp = tt_new(low->tt_arena, TT_BINARY, &TYPE_BOOL_INSTANCE, location);
        cmp->binary.op = TOKEN_EQUAL_EQUAL;
        cmp->binary.left = operand_ref;
        cmp->binary.right = lit;
        return cmp;
    }

    case PATTERN_RANGE: {
        TtNode *start = lower_expression(low, pattern->range_start);
        TtNode *end = lower_expression(low, pattern->range_end);

        TtNode *ge = tt_new(low->tt_arena, TT_BINARY, &TYPE_BOOL_INSTANCE, location);
        ge->binary.op = TOKEN_GREATER_EQUAL;
        ge->binary.left = operand_ref;
        ge->binary.right = start;

        TtNode *upper = tt_new(low->tt_arena, TT_BINARY, &TYPE_BOOL_INSTANCE, location);
        upper->binary.op = pattern->range_inclusive ? TOKEN_LESS_EQUAL : TOKEN_LESS;
        upper->binary.left = operand_ref;
        upper->binary.right = end;

        TtNode *combined = tt_new(low->tt_arena, TT_BINARY, &TYPE_BOOL_INSTANCE, location);
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
        TtNode *tag_access =
            tt_new(low->tt_arena, TT_STRUCT_FIELD_ACCESS, &TYPE_I32_INSTANCE, location);
        tag_access->struct_field_access.object = operand_ref;
        tag_access->struct_field_access.field = "_tag";
        tag_access->struct_field_access.via_pointer = false;

        TtNode *tag_val =
            lowering_make_int_lit(low, &(IntLitSpec){(uint64_t)variant->discriminant,
                                                     &TYPE_I32_INSTANCE, TYPE_I32, location});
        TtNode *cmp = tt_new(low->tt_arena, TT_BINARY, &TYPE_BOOL_INSTANCE, location);
        cmp->binary.op = TOKEN_EQUAL_EQUAL;
        cmp->binary.left = tag_access;
        cmp->binary.right = tag_val;
        return cmp;
    }
    }
    return NULL;
}

/** Register match arm bindings: extract variables from variant patterns. */
static void lower_pattern_bindings(Lowering *low, const ASTPattern *pattern, TtSymbol *operand_sym,
                                   const Type *operand_type) {
    (void)operand_sym;
    if (pattern->kind == PATTERN_BINDING && pattern->name != NULL) {
        TtSymbol *var_sym =
            lowering_add_variable(low, &(TtSymbolSpec){TT_SYMBOL_VARIABLE, pattern->name,
                                                       operand_type, false, pattern->location});
        (void)var_sym;
        return;
    }

    if (pattern->kind == PATTERN_VARIANT_TUPLE && operand_type != NULL &&
        operand_type->kind == TYPE_ENUM) {
        const EnumVariant *variant = type_enum_find_variant(operand_type, pattern->name);
        if (variant == NULL) {
            return;
        }
        for (int32_t i = 0; i < BUFFER_LENGTH(pattern->sub_patterns); i++) {
            ASTPattern *sub = pattern->sub_patterns[i];
            if (sub->kind == PATTERN_BINDING && sub->name != NULL && i < variant->tuple_count) {
                lowering_add_variable(low, &(TtSymbolSpec){TT_SYMBOL_VARIABLE, sub->name,
                                                           variant->tuple_types[i], false,
                                                           sub->location});
            }
        }
    }

    if (pattern->kind == PATTERN_VARIANT_STRUCT && operand_type != NULL &&
        operand_type->kind == TYPE_ENUM) {
        const EnumVariant *variant = type_enum_find_variant(operand_type, pattern->name);
        if (variant == NULL) {
            return;
        }
        for (int32_t i = 0; i < BUFFER_LENGTH(pattern->field_names); i++) {
            const char *fname = pattern->field_names[i];
            for (int32_t j = 0; j < variant->field_count; j++) {
                if (strcmp(variant->fields[j].name, fname) == 0) {
                    lowering_add_variable(low, &(TtSymbolSpec){TT_SYMBOL_VARIABLE, fname,
                                                               variant->fields[j].type, false,
                                                               pattern->location});
                    break;
                }
            }
        }
    }
}

/**
 * Build the bindings block for a single match arm.
 *
 * Extracts variables from the pattern and initializes them from the
 * match operand.  Returns NULL when no bindings are needed.
 */
static TtNode *lower_arm_bindings_block(Lowering *low, const ASTPattern *pattern,
                                        TtSymbol *operand_sym, const Type *operand_type,
                                        SourceLocation loc) {
    TtNode **bind_stmts = NULL;

    switch (pattern->kind) {
    case PATTERN_BINDING: {
        TtSymbol *bsym = lowering_scope_find(low, pattern->name);
        if (bsym != NULL) {
            TtNode *init = lowering_make_var_ref(low, operand_sym, loc);
            BUFFER_PUSH(bind_stmts, lowering_make_var_decl(low, bsym, init));
        }
        break;
    }
    case PATTERN_VARIANT_TUPLE: {
        const EnumVariant *variant = type_enum_find_variant(operand_type, pattern->name);
        if (variant == NULL) {
            break;
        }
        for (int32_t j = 0; j < BUFFER_LENGTH(pattern->sub_patterns); j++) {
            ASTPattern *sub = pattern->sub_patterns[j];
            if (sub->kind != PATTERN_BINDING || sub->name == NULL || j >= variant->tuple_count) {
                continue;
            }
            TtSymbol *bsym = lowering_scope_find(low, sub->name);
            if (bsym == NULL) {
                continue;
            }
            TtNode *data_access =
                tt_new(low->tt_arena, TT_STRUCT_FIELD_ACCESS, variant->tuple_types[j], loc);
            data_access->struct_field_access.object = lowering_make_var_ref(low, operand_sym, loc);
            data_access->struct_field_access.field =
                arena_sprintf(low->tt_arena, "_data.%s._%d", pattern->name, j);
            data_access->struct_field_access.via_pointer = false;
            BUFFER_PUSH(bind_stmts, lowering_make_var_decl(low, bsym, data_access));
        }
        break;
    }
    case PATTERN_VARIANT_STRUCT: {
        const EnumVariant *variant = type_enum_find_variant(operand_type, pattern->name);
        if (variant == NULL) {
            break;
        }
        for (int32_t j = 0; j < BUFFER_LENGTH(pattern->field_names); j++) {
            const char *fname = pattern->field_names[j];
            TtSymbol *bsym = lowering_scope_find(low, fname);
            if (bsym == NULL) {
                continue;
            }
            const Type *ftype = tt_symbol_type(bsym);
            TtNode *data_access = tt_new(low->tt_arena, TT_STRUCT_FIELD_ACCESS, ftype, loc);
            data_access->struct_field_access.object = lowering_make_var_ref(low, operand_sym, loc);
            data_access->struct_field_access.field =
                arena_sprintf(low->tt_arena, "_data.%s.%s", pattern->name, fname);
            data_access->struct_field_access.via_pointer = false;
            BUFFER_PUSH(bind_stmts, lowering_make_var_decl(low, bsym, data_access));
        }
        break;
    }
    default:
        return NULL;
    }

    if (BUFFER_LENGTH(bind_stmts) == 0) {
        return NULL;
    }
    TtNode *block = tt_new(low->tt_arena, TT_BLOCK, &TYPE_UNIT_INSTANCE, loc);
    block->block.statements = bind_stmts;
    block->block.result = NULL;
    return block;
}

TtNode *lower_match(Lowering *low, const ASTNode *ast) {
    TtNode *operand = lower_expression(low, ast->match_expression.operand);
    const Type *operand_type = operand->type;
    SourceLocation loc = ast->location;

    // Store operand in a temporary to avoid re-evaluation
    const char *match_tmp = lowering_make_temp_name(low);
    TtSymbolSpec match_spec = {TT_SYMBOL_VARIABLE, match_tmp, operand_type, false, loc};
    TtSymbol *match_sym = lowering_add_variable(low, &match_spec);

    TtNode **arm_conditions = NULL;
    TtNode **arm_guards = NULL;
    TtNode **arm_bodies = NULL;
    TtNode **arm_bindings = NULL;

    for (int32_t i = 0; i < BUFFER_LENGTH(ast->match_expression.arms); i++) {
        const ASTMatchArm *arm = &ast->match_expression.arms[i];

        TtNode *operand_ref = lowering_make_var_ref(low, match_sym, loc);

        TtNode *condition = lower_pattern_condition(low, arm->pattern, operand_ref, operand_type,
                                                    arm->pattern->location);

        // Guard — bindings must be emitted before guard in codegen
        TtNode *guard = NULL;
        if (arm->guard != NULL) {
            lowering_scope_enter(low);
            lower_pattern_bindings(low, arm->pattern, match_sym, operand_type);
            guard = lower_expression(low, arm->guard);
            lowering_scope_leave(low);
        }

        BUFFER_PUSH(arm_conditions, condition);
        BUFFER_PUSH(arm_guards, guard);

        lowering_scope_enter(low);
        lower_pattern_bindings(low, arm->pattern, match_sym, operand_type);

        TtNode *body = lower_expression(low, arm->body);
        BUFFER_PUSH(arm_bodies, body);

        BUFFER_PUSH(arm_bindings,
                    lower_arm_bindings_block(low, arm->pattern, match_sym, operand_type, loc));
        lowering_scope_leave(low);
    }

    TtNode *match_node = tt_new(low->tt_arena, TT_MATCH, ast->type, loc);
    match_node->match_expr.operand = lowering_make_var_decl(low, match_sym, operand);
    match_node->match_expr.arm_conditions = arm_conditions;
    match_node->match_expr.arm_guards = arm_guards;
    match_node->match_expr.arm_bodies = arm_bodies;
    match_node->match_expr.arm_bindings = arm_bindings;
    return match_node;
}
