#include "_sema.h"

// ── Named-arg helpers ─────────────────────────────────────────────

/**
 * Reorder call args to match param poss using named labels.
 * Clears @p node->call.arg_names after reordering.
 */
static void reorder_named_args(Sema *analyzer, ASTNode *node, const FnSignature *sig) {
    int32_t arg_count = BUF_LEN(node->call.args);
    if (node->call.arg_names == NULL || arg_count == 0) {
        return;
    }
    ASTNode **reordered = NULL;
    for (int32_t i = 0; i < sig->param_count; i++) {
        BUF_PUSH(reordered, (ASTNode *)NULL);
    }
    for (int32_t i = 0; i < arg_count; i++) {
        const char *aname = node->call.arg_names[i];
        if (aname != NULL) {
            int32_t idx = -1;
            for (int32_t j = 0; j < sig->param_count; j++) {
                if (strcmp(sig->param_names[j], aname) == 0) {
                    idx = j;
                    break;
                }
            }
            if (idx < 0) {
                SEMA_ERR(analyzer, node->call.args[i]->loc, "no parameter named '%s'", aname);
            } else {
                reordered[idx] = node->call.args[i];
            }
        } else if (i < BUF_LEN(reordered)) {
            reordered[i] = node->call.args[i];
        }
    }
    node->call.args = reordered;
    node->call.arg_names = NULL;
}

/**
 * Validate arg count against @p sig and type-check each arg.
 * Promotes lit args to match the corresponding param type.
 */
static void check_call_args(Sema *analyzer, ASTNode *node, const FnSignature *sig) {
    int32_t arg_count = BUF_LEN(node->call.args);
    if (arg_count != sig->param_count) {
        SEMA_ERR(analyzer, node->loc, "expected %d args, got %d", sig->param_count, arg_count);
        return;
    }
    for (int32_t i = 0; i < arg_count; i++) {
        ASTNode *arg = node->call.args[i];
        if (arg == NULL) {
            continue;
        }
        const Type *param_type = sig->param_types[i];
        promote_lit(arg, param_type);
        const Type *arg_type = arg->type;
        if (arg_type != NULL && param_type != NULL && !type_equal(arg_type, param_type) &&
            arg_type->kind != TYPE_ERR && param_type->kind != TYPE_ERR) {
            SEMA_ERR(analyzer, arg->loc, "type mismatch: expected '%s', got '%s'",
                     type_name(analyzer->arena, param_type), type_name(analyzer->arena, arg_type));
        }
    }
}

// ── Operator classification ────────────────────────────────────────────

static bool binary_op_yields_bool(TokenKind op) {
    return (op >= TOKEN_EQUAL_EQUAL && op <= TOKEN_GREATER_EQUAL) ||
           op == TOKEN_AMPERSAND_AMPERSAND || op == TOKEN_PIPE_PIPE;
}

// ── Expression checkers ────────────────────────────────────────────────

const Type *check_lit(Sema *analyzer, ASTNode *node) {
    (void)analyzer;
    return lit_kind_to_type(node->lit.kind);
}

const Type *check_id(Sema *analyzer, ASTNode *node) {
    Sym *sym = scope_lookup(analyzer, node->id.name);
    if (sym == NULL) {
        SEMA_ERR(analyzer, node->loc, "undefined variable '%s'", node->id.name);
        return &TYPE_ERR_INST;
    }
    return sym->type;
}

const Type *check_unary(Sema *analyzer, ASTNode *node) {
    const Type *operand = check_node(analyzer, node->unary.operand);
    if (operand == NULL || operand->kind == TYPE_ERR) {
        return &TYPE_ERR_INST;
    }
    if (node->unary.op == TOKEN_BANG) {
        if (!type_equal(operand, &TYPE_BOOL_INST)) {
            SEMA_ERR(analyzer, node->loc, "'!' requires 'bool' operand, got '%s'",
                     type_name(analyzer->arena, operand));
            return &TYPE_ERR_INST;
        }
        return &TYPE_BOOL_INST;
    }
    if (node->unary.op == TOKEN_MINUS) {
        if (!type_is_numeric(operand)) {
            SEMA_ERR(analyzer, node->loc, "'-' requires numeric operand, got '%s'",
                     type_name(analyzer->arena, operand));
            return &TYPE_ERR_INST;
        }
        return operand;
    }
    return operand;
}

const Type *check_binary(Sema *analyzer, ASTNode *node) {
    const Type *left = check_node(analyzer, node->binary.left);
    const Type *right = check_node(analyzer, node->binary.right);

    if (left == NULL || right == NULL || left->kind == TYPE_ERR || right->kind == TYPE_ERR) {
        return binary_op_yields_bool(node->binary.op) ? &TYPE_BOOL_INST : &TYPE_ERR_INST;
    }

    // Promote integer/float lits to match the other side's type
    if (!type_equal(left, right)) {
        const Type *promoted;
        promoted = promote_lit(node->binary.left, right);
        if (promoted != NULL) {
            left = promoted;
        }
        promoted = promote_lit(node->binary.right, left);
        if (promoted != NULL) {
            right = promoted;
        }
    }

    // Check for type mismatch after promotion
    if (!type_equal(left, right)) {
        if (left->kind != TYPE_ERR && right->kind != TYPE_ERR) {
            SEMA_ERR(analyzer, node->loc, "type mismatch: '%s' and '%s'",
                     type_name(analyzer->arena, left), type_name(analyzer->arena, right));
        }
    }

    // Boolean operations require bool operands
    if (node->binary.op == TOKEN_AMPERSAND_AMPERSAND || node->binary.op == TOKEN_PIPE_PIPE) {
        return &TYPE_BOOL_INST;
    }

    // Comparison operators return bool
    switch (node->binary.op) {
    case TOKEN_EQUAL_EQUAL:
    case TOKEN_BANG_EQUAL:
    case TOKEN_LESS:
    case TOKEN_LESS_EQUAL:
    case TOKEN_GREATER:
    case TOKEN_GREATER_EQUAL:
        return &TYPE_BOOL_INST;
    default:
        break;
    }

    // Arithmetic returns the operand type
    return left;
}

/** Check an enum tuple variant construction: Enum.Variant(args). */
static const Type *check_enum_variant_call(Sema *analyzer, ASTNode *node, const Type *enum_type,
                                           const char *variant_name) {
    const EnumVariant *variant = type_enum_find_variant(enum_type, variant_name);
    if (variant == NULL || variant->kind != ENUM_VARIANT_TUPLE) {
        return NULL;
    }
    int32_t arg_count = BUF_LEN(node->call.args);
    if (arg_count != variant->tuple_count) {
        SEMA_ERR(analyzer, node->loc, "expected %d args for variant '%s', got %d",
                 variant->tuple_count, variant_name, arg_count);
    } else {
        for (int32_t i = 0; i < arg_count; i++) {
            promote_lit(node->call.args[i], variant->tuple_types[i]);
            const Type *arg_type = node->call.args[i]->type;
            if (arg_type != NULL && !type_equal(arg_type, variant->tuple_types[i]) &&
                arg_type->kind != TYPE_ERR) {
                SEMA_ERR(analyzer, node->call.args[i]->loc,
                         "type mismatch: expected '%s', got '%s'",
                         type_name(analyzer->arena, variant->tuple_types[i]),
                         type_name(analyzer->arena, arg_type));
            }
        }
    }
    node->type = enum_type;
    return enum_type;
}

/** Resolve a struct method call, including promoted methods from embedded structs. */
static const Type *check_struct_method_call(Sema *analyzer, ASTNode *node, const Type *struct_type,
                                            const char *method_name) {
    const char *method_key =
        arena_sprintf(analyzer->arena, "%s.%s", struct_type->struct_type.name, method_name);
    FnSignature *sig = sema_lookup_fn(analyzer, method_key);

    // If not found directly, check embedded structs for promoted methods
    if (sig == NULL) {
        StructDef *sdef = sema_lookup_struct(analyzer, struct_type->struct_type.name);
        if (sdef != NULL) {
            for (int32_t ei = 0; ei < BUF_LEN(sdef->embedded); ei++) {
                const char *embed_key =
                    arena_sprintf(analyzer->arena, "%s.%s", sdef->embedded[ei], method_name);
                sig = sema_lookup_fn(analyzer, embed_key);
                if (sig != NULL) {
                    break;
                }
            }
        }
    }
    if (sig == NULL) {
        return NULL;
    }
    for (int32_t i = 0; i < BUF_LEN(node->call.args); i++) {
        check_node(analyzer, node->call.args[i]);
    }
    reorder_named_args(analyzer, node, sig);
    check_call_args(analyzer, node, sig);
    node->type = sig->return_type;
    return sig->return_type;
}

/** Try to resolve a member call (enum variant, enum method, or struct method). */
static const Type *check_member_call(Sema *analyzer, ASTNode *node, const char **out_fn_name) {
    const Type *obj_type = check_node(analyzer, node->call.callee->member.object);
    const char *method_name = node->call.callee->member.member;

    // Auto-deref for ptr types
    if (obj_type != NULL && obj_type->kind == TYPE_PTR) {
        obj_type = obj_type->ptr.pointee;
    }

    if (obj_type != NULL && obj_type->kind == TYPE_ENUM) {
        const Type *result = check_enum_variant_call(analyzer, node, obj_type, method_name);
        if (result != NULL) {
            return result;
        }
        // Enum method call
        const char *method_key =
            arena_sprintf(analyzer->arena, "%s.%s", type_enum_name(obj_type), method_name);
        FnSignature *sig = sema_lookup_fn(analyzer, method_key);
        if (sig != NULL) {
            reorder_named_args(analyzer, node, sig);
            check_call_args(analyzer, node, sig);
            node->type = sig->return_type;
            return sig->return_type;
        }
    }

    if (obj_type != NULL && obj_type->kind == TYPE_STRUCT) {
        const Type *result = check_struct_method_call(analyzer, node, obj_type, method_name);
        if (result != NULL) {
            return result;
        }
    }

    *out_fn_name = method_name;
    return NULL;
}

const Type *check_call(Sema *analyzer, ASTNode *node) {
    const char *fn_name = NULL;
    if (node->call.callee->kind == NODE_ID) {
        fn_name = node->call.callee->id.name;
    } else if (node->call.callee->kind == NODE_MEMBER) {
        const Type *result = check_member_call(analyzer, node, &fn_name);
        if (result != NULL) {
            return result;
        }
    }

    // Check args
    for (int32_t i = 0; i < BUF_LEN(node->call.args); i++) {
        check_node(analyzer, node->call.args[i]);
    }

    // Built-in fns
    if (fn_name != NULL && strcmp(fn_name, "assert") == 0) {
        return &TYPE_UNIT_INST;
    }

    // Look up fn return type and check arg types
    if (fn_name != NULL) {
        FnSignature *signature = sema_lookup_fn(analyzer, fn_name);
        if (signature != NULL) {
            reorder_named_args(analyzer, node, signature);
            check_call_args(analyzer, node, signature);
            return signature->return_type;
        }

        Sym *sym = scope_lookup(analyzer, fn_name);
        if (sym != NULL && sym->kind == SYM_FN) {
            return sym->type;
        }
        if (sym == NULL) {
            SEMA_ERR(analyzer, node->loc, "undefined function '%s'", fn_name);
        }
    }
    return &TYPE_ERR_INST;
}

const Type *check_member(Sema *analyzer, ASTNode *node) {
    const Type *object_type = check_node(analyzer, node->member.object);

    // Auto-deref: if object is a ptr, unwrap to pointee
    if (object_type != NULL && object_type->kind == TYPE_PTR) {
        object_type = object_type->ptr.pointee;
    }

    // Slice .len access
    if (object_type != NULL && object_type->kind == TYPE_SLICE) {
        if (strcmp(node->member.member, "len") == 0) {
            return &TYPE_I32_INST;
        }
    }

    // Tuple field access: .0, .1, .2, ...
    if (object_type != NULL && object_type->kind == TYPE_TUPLE) {
        char *end = NULL;
        long idx = strtol(node->member.member, &end, 10);
        if (end != NULL && *end == '\0' && idx >= 0 && idx < object_type->tuple.count) {
            return object_type->tuple.elems[idx];
        }
    }
    // Struct field access
    if (object_type != NULL && object_type->kind == TYPE_STRUCT) {
        const char *field_name = node->member.member;

        // Check own fields (including embedded struct fields by name, e.g., e.Base)
        const StructField *sf = type_struct_find_field(object_type, field_name);
        if (sf != NULL) {
            return sf->type;
        }

        // Check promoted fields from embedded structs
        StructDef *sdef = sema_lookup_struct(analyzer, object_type->struct_type.name);
        if (sdef != NULL) {
            for (int32_t ei = 0; ei < BUF_LEN(sdef->embedded); ei++) {
                StructDef *embed_def = sema_lookup_struct(analyzer, sdef->embedded[ei]);
                if (embed_def != NULL) {
                    for (int32_t fi = 0; fi < BUF_LEN(embed_def->fields); fi++) {
                        if (strcmp(embed_def->fields[fi].name, field_name) == 0) {
                            return embed_def->fields[fi].type;
                        }
                    }
                }
            }
        }

        SEMA_ERR(analyzer, node->loc, "no field '%s' on type '%s'", field_name,
                 object_type->struct_type.name);
        return &TYPE_ERR_INST;
    }

    // Enum variant access: EnumType.Variant
    if (object_type != NULL && object_type->kind == TYPE_ENUM) {
        const EnumVariant *variant = type_enum_find_variant(object_type, node->member.member);
        if (variant != NULL) {
            return object_type;
        }
        SEMA_ERR(analyzer, node->loc, "no variant '%s' on enum '%s'", node->member.member,
                 type_enum_name(object_type));
        return &TYPE_ERR_INST;
    }

    return &TYPE_ERR_INST;
}

const Type *check_idx(Sema *analyzer, ASTNode *node) {
    const Type *object_type = check_node(analyzer, node->idx_access.object);
    check_node(analyzer, node->idx_access.idx);
    if (object_type != NULL && object_type->kind == TYPE_ARRAY) {
        return object_type->array.elem;
    }
    if (object_type != NULL && object_type->kind == TYPE_SLICE) {
        return object_type->slice.elem;
    }
    return &TYPE_ERR_INST;
}

const Type *check_type_conversion(Sema *analyzer, ASTNode *node) {
    check_node(analyzer, node->type_conversion.operand);
    const Type *target = resolve_ast_type(analyzer, &node->type_conversion.target_type);
    if (target == NULL) {
        return &TYPE_ERR_INST;
    }
    promote_lit(node->type_conversion.operand, target);
    return target;
}

const Type *check_str_interpolation(Sema *analyzer, ASTNode *node) {
    for (int32_t i = 0; i < BUF_LEN(node->str_interpolation.parts); i++) {
        check_node(analyzer, node->str_interpolation.parts[i]);
    }
    return &TYPE_STR_INST;
}

const Type *check_array_lit(Sema *analyzer, ASTNode *node) {
    const Type *elem_type = resolve_ast_type(analyzer, &node->array_lit.elem_type);
    for (int32_t i = 0; i < BUF_LEN(node->array_lit.elems); i++) {
        const Type *elem = check_node(analyzer, node->array_lit.elems[i]);
        if (elem_type == NULL && elem != NULL) {
            elem_type = elem;
        }
        if (elem_type != NULL) {
            promote_lit(node->array_lit.elems[i], elem_type);
        }
    }
    if (elem_type == NULL) {
        elem_type = &TYPE_I32_INST;
    }
    int32_t size = node->array_lit.size;
    if (size == 0) {
        size = BUF_LEN(node->array_lit.elems);
    }
    return type_create_array(analyzer->arena, elem_type, size);
}

const Type *check_slice_lit(Sema *analyzer, ASTNode *node) {
    const Type *elem_type = resolve_ast_type(analyzer, &node->slice_lit.elem_type);
    for (int32_t i = 0; i < BUF_LEN(node->slice_lit.elems); i++) {
        const Type *elem = check_node(analyzer, node->slice_lit.elems[i]);
        if (elem_type == NULL && elem != NULL) {
            elem_type = elem;
        }
        if (elem_type != NULL) {
            promote_lit(node->slice_lit.elems[i], elem_type);
        }
    }
    if (elem_type == NULL) {
        elem_type = &TYPE_I32_INST;
    }
    return type_create_slice(analyzer->arena, elem_type);
}

const Type *check_slice_expr(Sema *analyzer, ASTNode *node) {
    const Type *object_type = check_node(analyzer, node->slice_expr.object);
    if (node->slice_expr.start != NULL) {
        check_node(analyzer, node->slice_expr.start);
    }
    if (node->slice_expr.end != NULL) {
        check_node(analyzer, node->slice_expr.end);
    }
    if (object_type != NULL && object_type->kind == TYPE_ARRAY) {
        return type_create_slice(analyzer->arena, object_type->array.elem);
    }
    if (object_type != NULL && object_type->kind == TYPE_SLICE) {
        return type_create_slice(analyzer->arena, object_type->slice.elem);
    }
    return &TYPE_ERR_INST;
}

const Type *check_tuple_lit(Sema *analyzer, ASTNode *node) {
    const Type ** /* buf */ elem_types = NULL;
    for (int32_t i = 0; i < BUF_LEN(node->tuple_lit.elems); i++) {
        const Type *elem = check_node(analyzer, node->tuple_lit.elems[i]);
        BUF_PUSH(elem_types, elem);
    }
    const Type *result = type_create_tuple(analyzer->arena, elem_types, BUF_LEN(elem_types));
    return result;
}

/** Promote and type-check a field value against an expected type. */
static void check_field_match(Sema *analyzer, ASTNode *value_node, const Type *expected_type) {
    promote_lit(value_node, expected_type);
    const Type *actual_type = value_node->type;
    if (actual_type != NULL && expected_type != NULL && !type_equal(actual_type, expected_type) &&
        actual_type->kind != TYPE_ERR && expected_type->kind != TYPE_ERR) {
        SEMA_ERR(analyzer, value_node->loc, "type mismatch: expected '%s', got '%s'",
                 type_name(analyzer->arena, expected_type),
                 type_name(analyzer->arena, actual_type));
    }
}

const Type *check_struct_lit(Sema *analyzer, ASTNode *node) {
    const char *struct_name = node->struct_lit.name;
    StructDef *sdef = sema_lookup_struct(analyzer, struct_name);
    if (sdef == NULL) {
        SEMA_ERR(analyzer, node->loc, "unknown struct '%s'", struct_name);
        return &TYPE_ERR_INST;
    }

    // Check that all provided fields exist and have the right types
    int32_t provided_count = BUF_LEN(node->struct_lit.field_names);
    for (int32_t i = 0; i < provided_count; i++) {
        const char *fname = node->struct_lit.field_names[i];
        ASTNode *fvalue = node->struct_lit.field_values[i];
        check_node(analyzer, fvalue);

        // Look up the field in the struct def
        bool found = false;
        for (int32_t j = 0; j < BUF_LEN(sdef->fields); j++) {
            if (strcmp(sdef->fields[j].name, fname) == 0) {
                found = true;
                check_field_match(analyzer, fvalue, sdef->fields[j].type);
                break;
            }
        }
        if (!found) {
            SEMA_ERR(analyzer, node->loc, "no field '%s' on struct '%s'", fname, struct_name);
        }
    }

    // Check that all required fields (no default) are provided
    for (int32_t i = 0; i < BUF_LEN(sdef->fields); i++) {
        if (sdef->fields[i].default_value == NULL) {
            bool provided = false;
            for (int32_t j = 0; j < provided_count; j++) {
                if (strcmp(node->struct_lit.field_names[j], sdef->fields[i].name) == 0) {
                    provided = true;
                    break;
                }
            }
            if (!provided) {
                SEMA_ERR(analyzer, node->loc, "missing field '%s' in struct '%s'",
                         sdef->fields[i].name, struct_name);
            }
        }
    }

    // Fill in default values for unprovided fields
    for (int32_t i = 0; i < BUF_LEN(sdef->fields); i++) {
        if (sdef->fields[i].default_value != NULL) {
            bool provided = false;
            for (int32_t j = 0; j < BUF_LEN(node->struct_lit.field_names); j++) {
                if (strcmp(node->struct_lit.field_names[j], sdef->fields[i].name) == 0) {
                    provided = true;
                    break;
                }
            }
            if (!provided) {
                BUF_PUSH(node->struct_lit.field_names, sdef->fields[i].name);
                BUF_PUSH(node->struct_lit.field_values, sdef->fields[i].default_value);
            }
        }
    }

    return sdef->type;
}

const Type *check_address_of(Sema *analyzer, ASTNode *node) {
    const Type *inner_type = check_node(analyzer, node->address_of.operand);
    if (inner_type == NULL || inner_type->kind == TYPE_ERR) {
        return &TYPE_ERR_INST;
    }
    // Addressability: only vars and struct lits are addressable
    ASTNode *operand = node->address_of.operand;
    if (operand->kind != NODE_ID && operand->kind != NODE_STRUCT_LIT &&
        operand->kind != NODE_MEMBER && operand->kind != NODE_IDX) {
        SEMA_ERR(analyzer, node->loc, "cannot take address of rvalue");
        return &TYPE_ERR_INST;
    }
    return type_create_ptr(analyzer->arena, inner_type, false);
}

const Type *check_deref(Sema *analyzer, ASTNode *node) {
    const Type *inner_type = check_node(analyzer, node->deref.operand);
    if (inner_type == NULL || inner_type->kind == TYPE_ERR) {
        return &TYPE_ERR_INST;
    }
    if (inner_type->kind != TYPE_PTR) {
        SEMA_ERR(analyzer, node->loc, "cannot deref non-ptr type '%s'",
                 type_name(analyzer->arena, inner_type));
        return &TYPE_ERR_INST;
    }
    return inner_type->ptr.pointee;
}

// ── Enum-related checkers ──────────────────────────────────────────────

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

/** Mark a variant as covered if the pattern matches an enum variant by name. */
static void mark_variant_covered(const Type *operand_type, const char *name,
                                 bool *variant_covered) {
    if (variant_covered == NULL) {
        return;
    }
    int32_t idx = find_variant_idx(operand_type, name);
    if (idx >= 0) {
        variant_covered[idx] = true;
    }
}

/** Check a pattern and bind vars in the current scope. */
static void check_pattern(Sema *analyzer, ASTPattern *pattern, const Type *operand_type,
                          bool *variant_covered, bool *has_wildcard) {
    switch (pattern->kind) {
    case PATTERN_WILDCARD:
        *has_wildcard = true;
        break;

    case PATTERN_BINDING:
        // Check if this id matches a variant name
        if (operand_type != NULL && operand_type->kind == TYPE_ENUM) {
            int32_t idx = find_variant_idx(operand_type, pattern->name);
            if (idx >= 0) {
                pattern->kind = PATTERN_VARIANT_UNIT;
                if (variant_covered != NULL) {
                    variant_covered[idx] = true;
                }
                break;
            }
        }
        // It's a binding - define the var in current scope
        if (operand_type != NULL) {
            scope_define(analyzer, &(SymDef){pattern->name, operand_type, false, SYM_VAR});
        }
        break;

    case PATTERN_LIT:
        check_node(analyzer, pattern->lit);
        if (operand_type != NULL) {
            promote_lit(pattern->lit, operand_type);
        }
        break;

    case PATTERN_RANGE:
        check_node(analyzer, pattern->range_start);
        check_node(analyzer, pattern->range_end);
        if (operand_type != NULL) {
            promote_lit(pattern->range_start, operand_type);
            promote_lit(pattern->range_end, operand_type);
        }
        break;

    case PATTERN_VARIANT_UNIT:
        if (operand_type != NULL && operand_type->kind == TYPE_ENUM) {
            mark_variant_covered(operand_type, pattern->name, variant_covered);
        }
        break;

    case PATTERN_VARIANT_TUPLE:
        if (operand_type != NULL && operand_type->kind == TYPE_ENUM) {
            const EnumVariant *variant = type_enum_find_variant(operand_type, pattern->name);
            if (variant == NULL) {
                SEMA_ERR(analyzer, pattern->loc, "unknown variant '%s'", pattern->name);
                break;
            }
            mark_variant_covered(operand_type, pattern->name, variant_covered);
            // Bind sub-patterns to tuple elem types
            for (int32_t i = 0; i < BUF_LEN(pattern->sub_patterns); i++) {
                ASTPattern *sub = pattern->sub_patterns[i];
                if (sub->kind == PATTERN_BINDING && i < variant->tuple_count) {
                    scope_define(analyzer,
                                 &(SymDef){sub->name, variant->tuple_types[i], false, SYM_VAR});
                } else if (sub->kind == PATTERN_WILDCARD) {
                    // nothing to bind
                }
            }
        }
        break;

    case PATTERN_VARIANT_STRUCT:
        if (operand_type != NULL && operand_type->kind == TYPE_ENUM) {
            const EnumVariant *variant = type_enum_find_variant(operand_type, pattern->name);
            if (variant == NULL) {
                SEMA_ERR(analyzer, pattern->loc, "unknown variant '%s'", pattern->name);
                break;
            }
            mark_variant_covered(operand_type, pattern->name, variant_covered);
            // Bind field names to their types
            for (int32_t i = 0; i < BUF_LEN(pattern->field_names); i++) {
                const char *fname = pattern->field_names[i];
                for (int32_t j = 0; j < variant->field_count; j++) {
                    if (strcmp(variant->fields[j].name, fname) == 0) {
                        scope_define(analyzer,
                                     &(SymDef){fname, variant->fields[j].type, false, SYM_VAR});
                        break;
                    }
                }
            }
        }
        break;
    }
}

const Type *check_match(Sema *analyzer, ASTNode *node) {
    const Type *operand_type = check_node(analyzer, node->match_expr.operand);
    const Type *result_type = NULL;
    bool has_wildcard = false;
    bool *variant_covered = NULL;

    if (operand_type != NULL && operand_type->kind == TYPE_ENUM) {
        int32_t variant_count = type_enum_variant_count(operand_type);
        variant_covered = arena_alloc_zero(analyzer->arena, variant_count * sizeof(bool));
    }

    for (int32_t i = 0; i < BUF_LEN(node->match_expr.arms); i++) {
        ASTMatchArm *arm = &node->match_expr.arms[i];
        scope_push(analyzer, false);

        check_pattern(analyzer, arm->pattern, operand_type, variant_covered, &has_wildcard);

        if (arm->guard != NULL) {
            const Type *guard_type = check_node(analyzer, arm->guard);
            if (guard_type != NULL && !type_equal(guard_type, &TYPE_BOOL_INST) &&
                guard_type->kind != TYPE_ERR) {
                SEMA_ERR(analyzer, arm->guard->loc, "match guard must be 'bool'");
            }
        }

        const Type *arm_type = check_node(analyzer, arm->body);
        if (result_type == NULL && arm_type != NULL && arm_type->kind != TYPE_UNIT) {
            result_type = arm_type;
        }

        scope_pop(analyzer);
    }

    // Exhaustiveness check for enums
    if (operand_type != NULL && operand_type->kind == TYPE_ENUM && variant_covered != NULL &&
        !has_wildcard) {
        int32_t variant_count = type_enum_variant_count(operand_type);
        for (int32_t i = 0; i < variant_count; i++) {
            if (!variant_covered[i]) {
                SEMA_ERR(analyzer, node->loc, "non-exhaustive match: missing variant '%s'",
                         type_enum_variants(operand_type)[i].name);
                break;
            }
        }
    }

    return result_type != NULL ? result_type : &TYPE_UNIT_INST;
}

const Type *check_enum_init(Sema *analyzer, ASTNode *node) {
    EnumDef *edef = sema_lookup_enum(analyzer, node->enum_init.enum_name);
    if (edef == NULL) {
        SEMA_ERR(analyzer, node->loc, "unknown enum '%s'", node->enum_init.enum_name);
        return &TYPE_ERR_INST;
    }

    const EnumVariant *variant = type_enum_find_variant(edef->type, node->enum_init.variant_name);
    if (variant == NULL) {
        SEMA_ERR(analyzer, node->loc, "unknown variant '%s' on enum '%s'",
                 node->enum_init.variant_name, node->enum_init.enum_name);
        return &TYPE_ERR_INST;
    }

    if (variant->kind != ENUM_VARIANT_STRUCT) {
        SEMA_ERR(analyzer, node->loc, "variant '%s' is not a struct variant",
                 node->enum_init.variant_name);
        return &TYPE_ERR_INST;
    }

    // Check that all provided fields exist and have the right types
    int32_t provided_count = BUF_LEN(node->enum_init.field_names);
    for (int32_t i = 0; i < provided_count; i++) {
        const char *fname = node->enum_init.field_names[i];
        ASTNode *fvalue = node->enum_init.field_values[i];
        check_node(analyzer, fvalue);

        bool found = false;
        for (int32_t j = 0; j < variant->field_count; j++) {
            if (strcmp(variant->fields[j].name, fname) == 0) {
                found = true;
                check_field_match(analyzer, fvalue, variant->fields[j].type);
                break;
            }
        }
        if (!found) {
            SEMA_ERR(analyzer, node->loc, "no field '%s' on variant '%s'", fname,
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
            SEMA_ERR(analyzer, node->loc, "missing field '%s' in variant '%s'",
                     variant->fields[i].name, node->enum_init.variant_name);
        }
    }

    return edef->type;
}
