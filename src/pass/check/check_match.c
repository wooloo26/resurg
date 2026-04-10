#include "_check.h"

// ── Static helpers ─────────────────────────────────────────────────

/** Find the idx of a variant by name in an enum type. Returns -1 if not found. */
static int32_t find_variant_idx(const Type *enum_type, const char *name) {
    const EnumVariant *variants = type_enum_variants(enum_type);
    int32_t count = type_enum_variant_count(enum_type);
    for (int32_t i = 0; i < count; i++) {
        if (strcmp(variants[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/** Record variant coverage if the pattern matches an enum variant by name. */
static void record_variant_coverage(const Type *operand_type, const char *name,
                                    bool *variant_covered) {
    if (variant_covered == NULL) {
        return;
    }
    int32_t idx = find_variant_idx(operand_type, name);
    if (idx >= 0) {
        variant_covered[idx] = true;
    }
}

// ── Pattern checking ───────────────────────────────────────────────

/** Check a variant-tuple pattern: bind positional sub-patterns to tuple types. */
static void check_variant_tuple_pattern(Sema *sema, ASTPattern *pattern, const Type *operand_type,
                                        MatchCoverage *coverage) {
    if (operand_type == NULL || operand_type->kind != TYPE_ENUM) {
        return;
    }
    const EnumVariant *variant = type_enum_find_variant(operand_type, pattern->name);
    if (variant == NULL) {
        SEMA_ERR(sema, pattern->loc, "unknown variant '%s'", pattern->name);
        return;
    }
    record_variant_coverage(operand_type, pattern->name, coverage->variant_covered);
    for (int32_t i = 0; i < BUF_LEN(pattern->sub_patterns); i++) {
        ASTPattern *sub = pattern->sub_patterns[i];
        if (sub->kind == PATTERN_BINDING && i < variant->tuple_count) {
            scope_define(sema, &(SymDef){sub->name, variant->tuple_types[i], false, SYM_VAR});
        }
    }
}

/** Check a variant-struct pattern: bind named fields to their types. */
static void check_variant_struct_pattern(Sema *sema, ASTPattern *pattern, const Type *operand_type,
                                         MatchCoverage *coverage) {
    if (operand_type == NULL || operand_type->kind != TYPE_ENUM) {
        return;
    }
    const EnumVariant *variant = type_enum_find_variant(operand_type, pattern->name);
    if (variant == NULL) {
        SEMA_ERR(sema, pattern->loc, "unknown variant '%s'", pattern->name);
        return;
    }
    record_variant_coverage(operand_type, pattern->name, coverage->variant_covered);
    for (int32_t i = 0; i < BUF_LEN(pattern->field_names); i++) {
        const char *fname = pattern->field_names[i];
        for (int32_t j = 0; j < variant->field_count; j++) {
            if (strcmp(variant->fields[j].name, fname) == 0) {
                scope_define(sema, &(SymDef){fname, variant->fields[j].type, false, SYM_VAR});
                break;
            }
        }
    }
}

/** Check a pattern and bind vars in the current scope. */
void check_pattern(Sema *sema, ASTPattern *pattern, const Type *operand_type,
                   MatchCoverage *coverage) {
    switch (pattern->kind) {
    case PATTERN_WILDCARD:
        *coverage->has_wildcard = true;
        break;

    case PATTERN_BINDING:
        // Check if this id matches a variant name
        if (operand_type != NULL && operand_type->kind == TYPE_ENUM) {
            int32_t idx = find_variant_idx(operand_type, pattern->name);
            if (idx >= 0) {
                pattern->kind = PATTERN_VARIANT_UNIT;
                if (coverage->variant_covered != NULL) {
                    coverage->variant_covered[idx] = true;
                }
                break;
            }
        }
        // It's a binding - define the var in current scope
        if (operand_type != NULL) {
            scope_define(sema, &(SymDef){pattern->name, operand_type, false, SYM_VAR});
        }
        break;

    case PATTERN_LIT:
        check_node(sema, pattern->lit);
        if (operand_type != NULL) {
            promote_lit(pattern->lit, operand_type);
        }
        break;

    case PATTERN_RANGE:
        check_node(sema, pattern->range_start);
        check_node(sema, pattern->range_end);
        if (operand_type != NULL) {
            promote_lit(pattern->range_start, operand_type);
            promote_lit(pattern->range_end, operand_type);
        }
        break;

    case PATTERN_VARIANT_UNIT:
        if (operand_type != NULL && operand_type->kind == TYPE_ENUM) {
            record_variant_coverage(operand_type, pattern->name, coverage->variant_covered);
        }
        break;

    case PATTERN_VARIANT_TUPLE:
        check_variant_tuple_pattern(sema, pattern, operand_type, coverage);
        break;

    case PATTERN_VARIANT_STRUCT:
        check_variant_struct_pattern(sema, pattern, operand_type, coverage);
        break;
    }
}

// ── Match / Enum-init checkers ─────────────────────────────────────

const Type *check_match(Sema *sema, ASTNode *node) {
    const Type *operand_type = check_node(sema, node->match_expr.operand);
    const Type *result_type = NULL;
    bool has_wildcard = false;
    bool *variant_covered = NULL;

    // Auto-deref: unwrap *T for pattern matching
    if (operand_type != NULL && operand_type->kind == TYPE_PTR) {
        operand_type = operand_type->ptr.pointee;
    }

    if (operand_type != NULL && operand_type->kind == TYPE_ENUM) {
        int32_t variant_count = type_enum_variant_count(operand_type);
        variant_covered = arena_alloc_zero(sema->base.arena, variant_count * sizeof(bool));
    }

    for (int32_t i = 0; i < BUF_LEN(node->match_expr.arms); i++) {
        ASTMatchArm *arm = &node->match_expr.arms[i];
        scope_push(sema, false);

        check_pattern(sema, arm->pattern, operand_type,
                      &(MatchCoverage){variant_covered, &has_wildcard});

        if (arm->guard != NULL) {
            const Type *guard_type = check_node(sema, arm->guard);
            if (guard_type != NULL && !type_equal(guard_type, &TYPE_BOOL_INST) &&
                guard_type->kind != TYPE_ERR) {
                SEMA_ERR(sema, arm->guard->loc, "match guard must be 'bool'");
            }
        }

        const Type *arm_type = check_node(sema, arm->body);
        if (result_type == NULL && arm_type != NULL && arm_type->kind != TYPE_UNIT) {
            result_type = arm_type;
        }

        scope_pop(sema);
    }

    // Exhaustiveness check for enums
    if (operand_type != NULL && operand_type->kind == TYPE_ENUM && variant_covered != NULL &&
        !has_wildcard) {
        int32_t variant_count = type_enum_variant_count(operand_type);
        for (int32_t i = 0; i < variant_count; i++) {
            if (!variant_covered[i]) {
                SEMA_ERR(sema, node->loc, "non-exhaustive match: missing variant '%s'",
                         type_enum_variants(operand_type)[i].name);
                break;
            }
        }
    }

    return result_type != NULL ? result_type : &TYPE_UNIT_INST;
}

const Type *check_enum_init(Sema *sema, ASTNode *node) {
    const char *enum_name = node->enum_init.enum_name;
    EnumDef *edef = sema_lookup_enum(sema, enum_name);

    // If not found and has type args, try to instantiate from generic template
    if (edef == NULL && BUF_LEN(node->enum_init.type_args) > 0) {
        GenericEnumDef *gdef = sema_lookup_generic_enum(sema, enum_name);
        if (gdef != NULL) {
            GenericInstArgs inst_args = {node->enum_init.type_args,
                                         BUF_LEN(node->enum_init.type_args), node->loc};
            const char *mangled = instantiate_generic_enum(sema, gdef, &inst_args);
            if (mangled != NULL) {
                node->enum_init.enum_name = mangled;
                enum_name = mangled;
                edef = sema_lookup_enum(sema, mangled);
            }
        }
    }

    if (edef == NULL) {
        SEMA_ERR(sema, node->loc, "unknown enum '%s'", enum_name);
        return &TYPE_ERR_INST;
    }

    const EnumVariant *variant = type_enum_find_variant(edef->type, node->enum_init.variant_name);
    if (variant == NULL) {
        SEMA_ERR(sema, node->loc, "unknown variant '%s' on enum '%s'", node->enum_init.variant_name,
                 node->enum_init.enum_name);
        return &TYPE_ERR_INST;
    }

    if (variant->kind != ENUM_VARIANT_STRUCT) {
        SEMA_ERR(sema, node->loc, "variant '%s' is not a struct variant",
                 node->enum_init.variant_name);
        return &TYPE_ERR_INST;
    }

    // Check that all provided fields exist and have the right types
    int32_t provided_count = BUF_LEN(node->enum_init.field_names);
    for (int32_t i = 0; i < provided_count; i++) {
        const char *fname = node->enum_init.field_names[i];
        ASTNode *fvalue = node->enum_init.field_values[i];
        check_node(sema, fvalue);

        bool found = false;
        for (int32_t j = 0; j < variant->field_count; j++) {
            if (strcmp(variant->fields[j].name, fname) == 0) {
                found = true;
                check_field_match(sema, fvalue, variant->fields[j].type);
                break;
            }
        }
        if (!found) {
            SEMA_ERR(sema, node->loc, "no field '%s' on variant '%s'", fname,
                     node->enum_init.variant_name);
        }
    }

    // Check that all required fields are provided
    for (int32_t i = 0; i < variant->field_count; i++) {
        bool provided = false;
        for (int32_t j = 0; j < provided_count; j++) {
            if (strcmp(node->enum_init.field_names[j], variant->fields[i].name) == 0) {
                provided = true;
                break;
            }
        }
        if (!provided) {
            SEMA_ERR(sema, node->loc, "missing field '%s' in variant '%s'", variant->fields[i].name,
                     node->enum_init.variant_name);
        }
    }

    return edef->type;
}
