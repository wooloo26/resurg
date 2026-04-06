#include "_sema.h"

// ── Named-argument helpers ─────────────────────────────────────────────

/**
 * Reorder call arguments to match parameter positions using named labels.
 * Clears @p node->call.arg_names after reordering.
 */
static void reorder_named_arguments(SemanticAnalyzer *analyzer, ASTNode *node,
                                    const FunctionSignature *sig) {
    int32_t arg_count = BUFFER_LENGTH(node->call.arguments);
    if (node->call.arg_names == NULL || arg_count == 0) {
        return;
    }
    ASTNode **reordered = NULL;
    for (int32_t i = 0; i < sig->parameter_count; i++) {
        BUFFER_PUSH(reordered, (ASTNode *)NULL);
    }
    for (int32_t i = 0; i < arg_count; i++) {
        const char *aname = node->call.arg_names[i];
        if (aname != NULL) {
            int32_t idx = -1;
            for (int32_t j = 0; j < sig->parameter_count; j++) {
                if (strcmp(sig->parameter_names[j], aname) == 0) {
                    idx = j;
                    break;
                }
            }
            if (idx < 0) {
                SEMA_ERROR(analyzer, node->call.arguments[i]->location, "no parameter named '%s'",
                           aname);
            } else {
                reordered[idx] = node->call.arguments[i];
            }
        } else if (i < BUFFER_LENGTH(reordered)) {
            reordered[i] = node->call.arguments[i];
        }
    }
    node->call.arguments = reordered;
    node->call.arg_names = NULL;
}

/**
 * Validate argument count against @p sig and type-check each argument.
 * Promotes literal arguments to match the corresponding parameter type.
 */
static void check_call_arguments(SemanticAnalyzer *analyzer, ASTNode *node,
                                 const FunctionSignature *sig) {
    int32_t arg_count = BUFFER_LENGTH(node->call.arguments);
    if (arg_count != sig->parameter_count) {
        SEMA_ERROR(analyzer, node->location, "expected %d arguments, got %d", sig->parameter_count,
                   arg_count);
        return;
    }
    for (int32_t i = 0; i < arg_count; i++) {
        ASTNode *arg = node->call.arguments[i];
        if (arg == NULL) {
            continue;
        }
        const Type *param_type = sig->parameter_types[i];
        promote_literal(arg, param_type);
        const Type *arg_type = arg->type;
        if (arg_type != NULL && param_type != NULL && !type_equal(arg_type, param_type) &&
            arg_type->kind != TYPE_ERROR && param_type->kind != TYPE_ERROR) {
            SEMA_ERROR(analyzer, arg->location, "type mismatch: expected '%s', got '%s'",
                       type_name(analyzer->arena, param_type),
                       type_name(analyzer->arena, arg_type));
        }
    }
}

// ── Operator classification ────────────────────────────────────────────

static bool binary_op_yields_bool(TokenKind op) {
    return (op >= TOKEN_EQUAL_EQUAL && op <= TOKEN_GREATER_EQUAL) ||
           op == TOKEN_AMPERSAND_AMPERSAND || op == TOKEN_PIPE_PIPE;
}

// ── Expression checkers ────────────────────────────────────────────────

const Type *check_literal(SemanticAnalyzer *analyzer, ASTNode *node) {
    (void)analyzer;
    return literal_kind_to_type(node->literal.kind);
}

const Type *check_identifier(SemanticAnalyzer *analyzer, ASTNode *node) {
    Symbol *symbol = scope_lookup(analyzer, node->identifier.name);
    if (symbol == NULL) {
        SEMA_ERROR(analyzer, node->location, "undefined variable '%s'", node->identifier.name);
        return &TYPE_ERROR_INSTANCE;
    }
    return symbol->type;
}

const Type *check_unary(SemanticAnalyzer *analyzer, ASTNode *node) {
    const Type *operand = check_node(analyzer, node->unary.operand);
    if (operand == NULL || operand->kind == TYPE_ERROR) {
        return &TYPE_ERROR_INSTANCE;
    }
    if (node->unary.op == TOKEN_BANG) {
        if (!type_equal(operand, &TYPE_BOOL_INSTANCE)) {
            SEMA_ERROR(analyzer, node->location, "'!' requires 'bool' operand, got '%s'",
                       type_name(analyzer->arena, operand));
            return &TYPE_ERROR_INSTANCE;
        }
        return &TYPE_BOOL_INSTANCE;
    }
    if (node->unary.op == TOKEN_MINUS) {
        if (!type_is_numeric(operand)) {
            SEMA_ERROR(analyzer, node->location, "'-' requires numeric operand, got '%s'",
                       type_name(analyzer->arena, operand));
            return &TYPE_ERROR_INSTANCE;
        }
        return operand;
    }
    return operand;
}

const Type *check_binary(SemanticAnalyzer *analyzer, ASTNode *node) {
    const Type *left = check_node(analyzer, node->binary.left);
    const Type *right = check_node(analyzer, node->binary.right);

    if (left == NULL || right == NULL || left->kind == TYPE_ERROR || right->kind == TYPE_ERROR) {
        return binary_op_yields_bool(node->binary.op) ? &TYPE_BOOL_INSTANCE : &TYPE_ERROR_INSTANCE;
    }

    // Promote integer/float literals to match the other side's type
    if (!type_equal(left, right)) {
        const Type *promoted;
        promoted = promote_literal(node->binary.left, right);
        if (promoted != NULL) {
            left = promoted;
        }
        promoted = promote_literal(node->binary.right, left);
        if (promoted != NULL) {
            right = promoted;
        }
    }

    // Check for type mismatch after promotion
    if (!type_equal(left, right)) {
        if (left->kind != TYPE_ERROR && right->kind != TYPE_ERROR) {
            SEMA_ERROR(analyzer, node->location, "type mismatch: '%s' and '%s'",
                       type_name(analyzer->arena, left), type_name(analyzer->arena, right));
        }
    }

    // Boolean operations require bool operands
    if (node->binary.op == TOKEN_AMPERSAND_AMPERSAND || node->binary.op == TOKEN_PIPE_PIPE) {
        return &TYPE_BOOL_INSTANCE;
    }

    // Comparison operators return bool
    switch (node->binary.op) {
    case TOKEN_EQUAL_EQUAL:
    case TOKEN_BANG_EQUAL:
    case TOKEN_LESS:
    case TOKEN_LESS_EQUAL:
    case TOKEN_GREATER:
    case TOKEN_GREATER_EQUAL:
        return &TYPE_BOOL_INSTANCE;
    default:
        break;
    }

    // Arithmetic returns the operand type
    return left;
}

/** Check an enum tuple variant construction: Enum.Variant(args). */
static const Type *check_enum_variant_call(SemanticAnalyzer *analyzer, ASTNode *node,
                                           const Type *enum_type, const char *variant_name) {
    const EnumVariant *variant = type_enum_find_variant(enum_type, variant_name);
    if (variant == NULL || variant->kind != ENUM_VARIANT_TUPLE) {
        return NULL;
    }
    int32_t arg_count = BUFFER_LENGTH(node->call.arguments);
    if (arg_count != variant->tuple_count) {
        SEMA_ERROR(analyzer, node->location, "expected %d arguments for variant '%s', got %d",
                   variant->tuple_count, variant_name, arg_count);
    } else {
        for (int32_t i = 0; i < arg_count; i++) {
            promote_literal(node->call.arguments[i], variant->tuple_types[i]);
            const Type *arg_type = node->call.arguments[i]->type;
            if (arg_type != NULL && !type_equal(arg_type, variant->tuple_types[i]) &&
                arg_type->kind != TYPE_ERROR) {
                SEMA_ERROR(analyzer, node->call.arguments[i]->location,
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
static const Type *check_struct_method_call(SemanticAnalyzer *analyzer, ASTNode *node,
                                            const Type *struct_type, const char *method_name) {
    const char *method_key =
        arena_sprintf(analyzer->arena, "%s.%s", struct_type->struct_type.name, method_name);
    FunctionSignature *sig = sema_lookup_function(analyzer, method_key);

    // If not found directly, check embedded structs for promoted methods
    if (sig == NULL) {
        StructDefinition *sdef = sema_lookup_struct(analyzer, struct_type->struct_type.name);
        if (sdef != NULL) {
            for (int32_t ei = 0; ei < BUFFER_LENGTH(sdef->embedded); ei++) {
                const char *embed_key =
                    arena_sprintf(analyzer->arena, "%s.%s", sdef->embedded[ei], method_name);
                sig = sema_lookup_function(analyzer, embed_key);
                if (sig != NULL) {
                    break;
                }
            }
        }
    }
    if (sig == NULL) {
        return NULL;
    }
    for (int32_t i = 0; i < BUFFER_LENGTH(node->call.arguments); i++) {
        check_node(analyzer, node->call.arguments[i]);
    }
    reorder_named_arguments(analyzer, node, sig);
    check_call_arguments(analyzer, node, sig);
    node->type = sig->return_type;
    return sig->return_type;
}

/** Try to resolve a member call (enum variant, enum method, or struct method). */
static const Type *check_member_call(SemanticAnalyzer *analyzer, ASTNode *node,
                                     const char **out_function_name) {
    const Type *obj_type = check_node(analyzer, node->call.callee->member.object);
    const char *method_name = node->call.callee->member.member;

    // Auto-deref for pointer types
    if (obj_type != NULL && obj_type->kind == TYPE_POINTER) {
        obj_type = obj_type->pointer.pointee;
    }

    if (obj_type != NULL && obj_type->kind == TYPE_ENUM) {
        const Type *result = check_enum_variant_call(analyzer, node, obj_type, method_name);
        if (result != NULL) {
            return result;
        }
        // Enum method call
        const char *method_key =
            arena_sprintf(analyzer->arena, "%s.%s", type_enum_name(obj_type), method_name);
        FunctionSignature *sig = sema_lookup_function(analyzer, method_key);
        if (sig != NULL) {
            reorder_named_arguments(analyzer, node, sig);
            check_call_arguments(analyzer, node, sig);
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

    *out_function_name = method_name;
    return NULL;
}

const Type *check_call(SemanticAnalyzer *analyzer, ASTNode *node) {
    const char *function_name = NULL;
    if (node->call.callee->kind == NODE_IDENTIFIER) {
        function_name = node->call.callee->identifier.name;
    } else if (node->call.callee->kind == NODE_MEMBER) {
        const Type *result = check_member_call(analyzer, node, &function_name);
        if (result != NULL) {
            return result;
        }
    }

    // Check arguments
    for (int32_t i = 0; i < BUFFER_LENGTH(node->call.arguments); i++) {
        check_node(analyzer, node->call.arguments[i]);
    }

    // Built-in functions
    if (function_name != NULL && strcmp(function_name, "assert") == 0) {
        return &TYPE_UNIT_INSTANCE;
    }

    // Look up function return type and check argument types
    if (function_name != NULL) {
        FunctionSignature *signature = sema_lookup_function(analyzer, function_name);
        if (signature != NULL) {
            reorder_named_arguments(analyzer, node, signature);
            check_call_arguments(analyzer, node, signature);
            return signature->return_type;
        }

        Symbol *symbol = scope_lookup(analyzer, function_name);
        if (symbol != NULL && symbol->kind == SYM_FUNCTION) {
            return symbol->type;
        }
        if (symbol == NULL) {
            SEMA_ERROR(analyzer, node->location, "undefined function '%s'", function_name);
        }
    }
    return &TYPE_ERROR_INSTANCE;
}

const Type *check_member(SemanticAnalyzer *analyzer, ASTNode *node) {
    const Type *object_type = check_node(analyzer, node->member.object);

    // Auto-deref: if object is a pointer, unwrap to pointee
    if (object_type != NULL && object_type->kind == TYPE_POINTER) {
        object_type = object_type->pointer.pointee;
    }

    // Tuple field access: .0, .1, .2, ...
    if (object_type != NULL && object_type->kind == TYPE_TUPLE) {
        char *end = NULL;
        long index = strtol(node->member.member, &end, 10);
        if (end != NULL && *end == '\0' && index >= 0 && index < object_type->tuple.count) {
            return object_type->tuple.elements[index];
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
        StructDefinition *sdef = sema_lookup_struct(analyzer, object_type->struct_type.name);
        if (sdef != NULL) {
            for (int32_t ei = 0; ei < BUFFER_LENGTH(sdef->embedded); ei++) {
                StructDefinition *embed_def = sema_lookup_struct(analyzer, sdef->embedded[ei]);
                if (embed_def != NULL) {
                    for (int32_t fi = 0; fi < BUFFER_LENGTH(embed_def->fields); fi++) {
                        if (strcmp(embed_def->fields[fi].name, field_name) == 0) {
                            return embed_def->fields[fi].type;
                        }
                    }
                }
            }
        }

        SEMA_ERROR(analyzer, node->location, "no field '%s' on type '%s'", field_name,
                   object_type->struct_type.name);
        return &TYPE_ERROR_INSTANCE;
    }

    // Enum variant access: EnumType.Variant
    if (object_type != NULL && object_type->kind == TYPE_ENUM) {
        const EnumVariant *variant = type_enum_find_variant(object_type, node->member.member);
        if (variant != NULL) {
            return object_type;
        }
        SEMA_ERROR(analyzer, node->location, "no variant '%s' on enum '%s'", node->member.member,
                   type_enum_name(object_type));
        return &TYPE_ERROR_INSTANCE;
    }

    return &TYPE_ERROR_INSTANCE;
}

const Type *check_index(SemanticAnalyzer *analyzer, ASTNode *node) {
    const Type *object_type = check_node(analyzer, node->index_access.object);
    check_node(analyzer, node->index_access.index);
    if (object_type != NULL && object_type->kind == TYPE_ARRAY) {
        return object_type->array.element;
    }
    return &TYPE_ERROR_INSTANCE;
}

const Type *check_type_conversion(SemanticAnalyzer *analyzer, ASTNode *node) {
    check_node(analyzer, node->type_conversion.operand);
    const Type *target = resolve_ast_type(analyzer, &node->type_conversion.target_type);
    if (target == NULL) {
        return &TYPE_ERROR_INSTANCE;
    }
    promote_literal(node->type_conversion.operand, target);
    return target;
}

const Type *check_string_interpolation(SemanticAnalyzer *analyzer, ASTNode *node) {
    for (int32_t i = 0; i < BUFFER_LENGTH(node->string_interpolation.parts); i++) {
        check_node(analyzer, node->string_interpolation.parts[i]);
    }
    return &TYPE_STRING_INSTANCE;
}

const Type *check_array_literal(SemanticAnalyzer *analyzer, ASTNode *node) {
    const Type *element_type = resolve_ast_type(analyzer, &node->array_literal.element_type);
    for (int32_t i = 0; i < BUFFER_LENGTH(node->array_literal.elements); i++) {
        const Type *elem = check_node(analyzer, node->array_literal.elements[i]);
        if (element_type == NULL && elem != NULL) {
            element_type = elem;
        }
        if (element_type != NULL) {
            promote_literal(node->array_literal.elements[i], element_type);
        }
    }
    if (element_type == NULL) {
        element_type = &TYPE_I32_INSTANCE;
    }
    int32_t size = node->array_literal.size;
    if (size == 0) {
        size = BUFFER_LENGTH(node->array_literal.elements);
    }
    return type_create_array(analyzer->arena, element_type, size);
}

const Type *check_tuple_literal(SemanticAnalyzer *analyzer, ASTNode *node) {
    const Type ** /* buf */ element_types = NULL;
    for (int32_t i = 0; i < BUFFER_LENGTH(node->tuple_literal.elements); i++) {
        const Type *elem = check_node(analyzer, node->tuple_literal.elements[i]);
        BUFFER_PUSH(element_types, elem);
    }
    const Type *result =
        type_create_tuple(analyzer->arena, element_types, BUFFER_LENGTH(element_types));
    return result;
}

/** Promote and type-check a field value against an expected type. */
static void check_field_match(SemanticAnalyzer *analyzer, ASTNode *value_node,
                              const Type *expected_type) {
    promote_literal(value_node, expected_type);
    const Type *actual_type = value_node->type;
    if (actual_type != NULL && expected_type != NULL && !type_equal(actual_type, expected_type) &&
        actual_type->kind != TYPE_ERROR && expected_type->kind != TYPE_ERROR) {
        SEMA_ERROR(analyzer, value_node->location, "type mismatch: expected '%s', got '%s'",
                   type_name(analyzer->arena, expected_type),
                   type_name(analyzer->arena, actual_type));
    }
}

const Type *check_struct_literal(SemanticAnalyzer *analyzer, ASTNode *node) {
    const char *struct_name = node->struct_literal.name;
    StructDefinition *sdef = sema_lookup_struct(analyzer, struct_name);
    if (sdef == NULL) {
        SEMA_ERROR(analyzer, node->location, "unknown struct '%s'", struct_name);
        return &TYPE_ERROR_INSTANCE;
    }

    // Check that all provided fields exist and have the right types
    int32_t provided_count = BUFFER_LENGTH(node->struct_literal.field_names);
    for (int32_t i = 0; i < provided_count; i++) {
        const char *fname = node->struct_literal.field_names[i];
        ASTNode *fvalue = node->struct_literal.field_values[i];
        check_node(analyzer, fvalue);

        // Look up the field in the struct definition
        bool found = false;
        for (int32_t j = 0; j < BUFFER_LENGTH(sdef->fields); j++) {
            if (strcmp(sdef->fields[j].name, fname) == 0) {
                found = true;
                check_field_match(analyzer, fvalue, sdef->fields[j].type);
                break;
            }
        }
        if (!found) {
            SEMA_ERROR(analyzer, node->location, "no field '%s' on struct '%s'", fname,
                       struct_name);
        }
    }

    // Check that all required fields (no default) are provided
    for (int32_t i = 0; i < BUFFER_LENGTH(sdef->fields); i++) {
        if (sdef->fields[i].default_value == NULL) {
            bool provided = false;
            for (int32_t j = 0; j < provided_count; j++) {
                if (strcmp(node->struct_literal.field_names[j], sdef->fields[i].name) == 0) {
                    provided = true;
                    break;
                }
            }
            if (!provided) {
                SEMA_ERROR(analyzer, node->location, "missing field '%s' in struct '%s'",
                           sdef->fields[i].name, struct_name);
            }
        }
    }

    // Fill in default values for unprovided fields
    for (int32_t i = 0; i < BUFFER_LENGTH(sdef->fields); i++) {
        if (sdef->fields[i].default_value != NULL) {
            bool provided = false;
            for (int32_t j = 0; j < BUFFER_LENGTH(node->struct_literal.field_names); j++) {
                if (strcmp(node->struct_literal.field_names[j], sdef->fields[i].name) == 0) {
                    provided = true;
                    break;
                }
            }
            if (!provided) {
                BUFFER_PUSH(node->struct_literal.field_names, sdef->fields[i].name);
                BUFFER_PUSH(node->struct_literal.field_values, sdef->fields[i].default_value);
            }
        }
    }

    return sdef->type;
}

const Type *check_address_of(SemanticAnalyzer *analyzer, ASTNode *node) {
    const Type *inner_type = check_node(analyzer, node->address_of.operand);
    if (inner_type == NULL || inner_type->kind == TYPE_ERROR) {
        return &TYPE_ERROR_INSTANCE;
    }
    // Addressability: only variables and struct literals are addressable
    ASTNode *operand = node->address_of.operand;
    if (operand->kind != NODE_IDENTIFIER && operand->kind != NODE_STRUCT_LITERAL &&
        operand->kind != NODE_MEMBER && operand->kind != NODE_INDEX) {
        SEMA_ERROR(analyzer, node->location, "cannot take address of rvalue");
        return &TYPE_ERROR_INSTANCE;
    }
    return type_create_pointer(analyzer->arena, inner_type, false);
}

const Type *check_deref(SemanticAnalyzer *analyzer, ASTNode *node) {
    const Type *inner_type = check_node(analyzer, node->deref.operand);
    if (inner_type == NULL || inner_type->kind == TYPE_ERROR) {
        return &TYPE_ERROR_INSTANCE;
    }
    if (inner_type->kind != TYPE_POINTER) {
        SEMA_ERROR(analyzer, node->location, "cannot dereference non-pointer type '%s'",
                   type_name(analyzer->arena, inner_type));
        return &TYPE_ERROR_INSTANCE;
    }
    return inner_type->pointer.pointee;
}

// ── Enum-related checkers ──────────────────────────────────────────────

/** Find the index of a variant by name in an enum type. Returns -1 if not found. */
static int32_t find_variant_index(const Type *enum_type, const char *name) {
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
    int32_t idx = find_variant_index(operand_type, name);
    if (idx >= 0) {
        variant_covered[idx] = true;
    }
}

/** Check a pattern and bind variables in the current scope. */
static void check_pattern(SemanticAnalyzer *analyzer, ASTPattern *pattern, const Type *operand_type,
                          bool *variant_covered, bool *has_wildcard) {
    switch (pattern->kind) {
    case PATTERN_WILDCARD:
        *has_wildcard = true;
        break;

    case PATTERN_BINDING:
        // Check if this identifier matches a variant name
        if (operand_type != NULL && operand_type->kind == TYPE_ENUM) {
            int32_t idx = find_variant_index(operand_type, pattern->name);
            if (idx >= 0) {
                pattern->kind = PATTERN_VARIANT_UNIT;
                if (variant_covered != NULL) {
                    variant_covered[idx] = true;
                }
                break;
            }
        }
        // It's a binding - define the variable in current scope
        if (operand_type != NULL) {
            scope_define(analyzer, &(SymbolDef){pattern->name, operand_type, false, SYM_VAR});
        }
        break;

    case PATTERN_LITERAL:
        check_node(analyzer, pattern->literal);
        if (operand_type != NULL) {
            promote_literal(pattern->literal, operand_type);
        }
        break;

    case PATTERN_RANGE:
        check_node(analyzer, pattern->range_start);
        check_node(analyzer, pattern->range_end);
        if (operand_type != NULL) {
            promote_literal(pattern->range_start, operand_type);
            promote_literal(pattern->range_end, operand_type);
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
                SEMA_ERROR(analyzer, pattern->location, "unknown variant '%s'", pattern->name);
                break;
            }
            mark_variant_covered(operand_type, pattern->name, variant_covered);
            // Bind sub-patterns to tuple element types
            for (int32_t i = 0; i < BUFFER_LENGTH(pattern->sub_patterns); i++) {
                ASTPattern *sub = pattern->sub_patterns[i];
                if (sub->kind == PATTERN_BINDING && i < variant->tuple_count) {
                    scope_define(analyzer,
                                 &(SymbolDef){sub->name, variant->tuple_types[i], false, SYM_VAR});
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
                SEMA_ERROR(analyzer, pattern->location, "unknown variant '%s'", pattern->name);
                break;
            }
            mark_variant_covered(operand_type, pattern->name, variant_covered);
            // Bind field names to their types
            for (int32_t i = 0; i < BUFFER_LENGTH(pattern->field_names); i++) {
                const char *fname = pattern->field_names[i];
                for (int32_t j = 0; j < variant->field_count; j++) {
                    if (strcmp(variant->fields[j].name, fname) == 0) {
                        scope_define(analyzer,
                                     &(SymbolDef){fname, variant->fields[j].type, false, SYM_VAR});
                        break;
                    }
                }
            }
        }
        break;
    }
}

const Type *check_match(SemanticAnalyzer *analyzer, ASTNode *node) {
    const Type *operand_type = check_node(analyzer, node->match_expression.operand);
    const Type *result_type = NULL;
    bool has_wildcard = false;
    bool *variant_covered = NULL;

    if (operand_type != NULL && operand_type->kind == TYPE_ENUM) {
        int32_t variant_count = type_enum_variant_count(operand_type);
        variant_covered = arena_alloc_zero(analyzer->arena, variant_count * sizeof(bool));
    }

    for (int32_t i = 0; i < BUFFER_LENGTH(node->match_expression.arms); i++) {
        ASTMatchArm *arm = &node->match_expression.arms[i];
        scope_push(analyzer, false);

        check_pattern(analyzer, arm->pattern, operand_type, variant_covered, &has_wildcard);

        if (arm->guard != NULL) {
            const Type *guard_type = check_node(analyzer, arm->guard);
            if (guard_type != NULL && !type_equal(guard_type, &TYPE_BOOL_INSTANCE) &&
                guard_type->kind != TYPE_ERROR) {
                SEMA_ERROR(analyzer, arm->guard->location, "match guard must be 'bool'");
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
                SEMA_ERROR(analyzer, node->location, "non-exhaustive match: missing variant '%s'",
                           type_enum_variants(operand_type)[i].name);
                break;
            }
        }
    }

    return result_type != NULL ? result_type : &TYPE_UNIT_INSTANCE;
}

const Type *check_enum_init(SemanticAnalyzer *analyzer, ASTNode *node) {
    EnumDefinition *edef = sema_lookup_enum(analyzer, node->enum_init.enum_name);
    if (edef == NULL) {
        SEMA_ERROR(analyzer, node->location, "unknown enum '%s'", node->enum_init.enum_name);
        return &TYPE_ERROR_INSTANCE;
    }

    const EnumVariant *variant = type_enum_find_variant(edef->type, node->enum_init.variant_name);
    if (variant == NULL) {
        SEMA_ERROR(analyzer, node->location, "unknown variant '%s' on enum '%s'",
                   node->enum_init.variant_name, node->enum_init.enum_name);
        return &TYPE_ERROR_INSTANCE;
    }

    if (variant->kind != ENUM_VARIANT_STRUCT) {
        SEMA_ERROR(analyzer, node->location, "variant '%s' is not a struct variant",
                   node->enum_init.variant_name);
        return &TYPE_ERROR_INSTANCE;
    }

    // Check that all provided fields exist and have the right types
    int32_t provided_count = BUFFER_LENGTH(node->enum_init.field_names);
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
            SEMA_ERROR(analyzer, node->location, "no field '%s' on variant '%s'", fname,
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
            SEMA_ERROR(analyzer, node->location, "missing field '%s' in variant '%s'",
                       variant->fields[i].name, node->enum_init.variant_name);
        }
    }

    return edef->type;
}
