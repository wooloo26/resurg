#include "_lower.h"

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
    if (state.variant == NULL) {
        return hir_new(low->hir_arena, HIR_UNIT_LIT, &TYPE_ERR_INST, spec->loc);
    }
    return finish_enum_init(low, spec, &state);
}

HirNode *lower_enum_tuple_init(Lower *low, const EnumVariantSpec *spec, ASTNode **args) {
    EnumInitState state = begin_enum_init(low, spec);
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
