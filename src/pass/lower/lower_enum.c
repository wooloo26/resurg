#include "_lower.h"

// ── Enum desugaring helpers ────────────────────────────────────────

/** Build a tag comparison: @p sym._tag == @p variant->discriminant. */
HirNode *lower_enum_tag_check(Lower *low, HirSym *sym, const EnumVariant *variant, SrcLoc loc) {
    HirNode *tag =
        lower_make_field_access(low, &(FieldAccessSpec){lower_make_var_ref(low, sym, loc), "_tag",
                                                        &TYPE_I32_INST, false, loc});
    HirNode *tag_val = lower_make_int_lit(
        low, &(IntLitSpec){(uint64_t)variant->discriminant, &TYPE_I32_INST, TYPE_I32, loc});
    HirNode *cond = hir_new(low->hir_arena, HIR_BINARY, &TYPE_BOOL_INST, loc);
    cond->binary.op = TOKEN_EQUAL_EQUAL;
    cond->binary.left = tag;
    cond->binary.right = tag_val;
    return cond;
}

/**
 * Build an enum variant struct literal with the given discriminant tag.
 * If @p data_field is non-NULL, the literal also contains the data payload.
 */
HirNode *lower_enum_variant_lit(Lower *low, const EnumVariant *variant, const char *data_field,
                                HirNode *data_val, const Type *result_type, SrcLoc loc) {
    const char **fn = NULL;
    HirNode **fv = NULL;
    BUF_PUSH(fn, "_tag");
    BUF_PUSH(fv, lower_make_int_lit(low, &(IntLitSpec){(uint64_t)variant->discriminant,
                                                       &TYPE_I32_INST, TYPE_I32, loc}));
    if (data_field != NULL) {
        BUF_PUSH(fn, data_field);
        BUF_PUSH(fv, data_val);
    }
    HirNode *lit = hir_new(low->hir_arena, HIR_STRUCT_LIT, result_type, loc);
    lit->struct_lit.field_names = fn;
    lit->struct_lit.field_values = fv;
    return lit;
}

// ── Enum variant lowering ──────────────────────────────────────────

/** Internal state for building an enum variant struct literal. */
typedef struct {
    const EnumVariant *variant;
    const char **field_names;
    HirNode **field_values;
} EnumInitState;

/**
 * Start an enum variant init: look up the variant, prepare _tag.
 *
 * Returns a state with variant, field_names and field_values bufs
 * (with _tag prepopulated).  Returns variant==NULL on lookup failure.
 */
static EnumInitState begin_enum_init(Lower *low, const EnumVariantSpec *spec) {
    EnumInitState state = {0};
    state.variant = type_enum_find_variant(spec->enum_type, spec->variant_name);
    if (state.variant == NULL) {
        return state;
    }

    BUF_PUSH(state.field_names, "_tag");
    BUF_PUSH(state.field_values,
             lower_make_int_lit(low, &(IntLitSpec){(uint64_t)state.variant->discriminant,
                                                   &TYPE_I32_INST, TYPE_I32, spec->loc}));
    return state;
}

/** Finish an enum variant init: build the HIR_STRUCT_LIT node. */
static HirNode *finish_enum_init(Lower *low, const EnumVariantSpec *spec,
                                 const EnumInitState *state) {
    HirNode *node = hir_new(low->hir_arena, HIR_STRUCT_LIT, spec->enum_type, spec->loc);
    node->struct_lit.field_names = state->field_names;
    node->struct_lit.field_values = state->field_values;

    BUF_PUSH(low->compound_types, spec->enum_type);
    return node;
}

HirNode *lower_enum_unit_init(Lower *low, const EnumVariantSpec *spec) {
    EnumInitState state = begin_enum_init(low, spec);
    assert(state.variant != NULL && "lower: enum unit variant must be resolved after check");
    if (state.variant == NULL) {
        return hir_new(low->hir_arena, HIR_UNIT_LIT, &TYPE_ERR_INST, spec->loc);
    }
    return finish_enum_init(low, spec, &state);
}

HirNode *lower_enum_tuple_init(Lower *low, const EnumVariantSpec *spec, ASTNode **args) {
    EnumInitState state = begin_enum_init(low, spec);
    assert(state.variant != NULL && "lower: enum tuple variant must be resolved after check");
    if (state.variant == NULL) {
        return hir_new(low->hir_arena, HIR_UNIT_LIT, &TYPE_ERR_INST, spec->loc);
    }

    for (int32_t i = 0; i < BUF_LEN(args); i++) {
        // Skip unit-typed fields — they have no C representation
        if (state.variant->tuple_types[i]->kind == TYPE_UNIT) {
            continue;
        }
        HirNode *val = lower_expr(low, args[i]);
        const char *fname = arena_sprintf(low->hir_arena, "_data.%s._%d", spec->variant_name, i);
        BUF_PUSH(state.field_names, fname);
        BUF_PUSH(state.field_values, val);
    }
    return finish_enum_init(low, spec, &state);
}

HirNode *lower_enum_struct_init(Lower *low, const EnumVariantSpec *spec,
                                const char **ast_field_names, ASTNode **ast_field_values) {
    EnumInitState state = begin_enum_init(low, spec);
    assert(state.variant != NULL && "lower: enum struct variant must be resolved after check");
    if (state.variant == NULL) {
        return hir_new(low->hir_arena, HIR_UNIT_LIT, &TYPE_ERR_INST, spec->loc);
    }

    for (int32_t i = 0; i < BUF_LEN(ast_field_names); i++) {
        const char *fname =
            arena_sprintf(low->hir_arena, "_data.%s.%s", spec->variant_name, ast_field_names[i]);
        BUF_PUSH(state.field_names, fname);
        BUF_PUSH(state.field_values, lower_expr(low, ast_field_values[i]));
    }
    return finish_enum_init(low, spec, &state);
}
