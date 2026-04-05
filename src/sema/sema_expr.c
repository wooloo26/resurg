#include "_sema.h"

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

const Type *check_call(SemanticAnalyzer *analyzer, ASTNode *node) {
    // Check callee
    const char *function_name = NULL;
    if (node->call.callee->kind == NODE_IDENTIFIER) {
        function_name = node->call.callee->identifier.name;
    } else if (node->call.callee->kind == NODE_MEMBER) {
        // Could be a method call — check the object's type
        const Type *obj_type = check_node(analyzer, node->call.callee->member.object);
        const char *method_name = node->call.callee->member.member;

        if (obj_type != NULL && obj_type->kind == TYPE_STRUCT) {
            // Look up method on this struct (or promoted from embedded)
            const char *method_key =
                arena_sprintf(analyzer->arena, "%s.%s", obj_type->struct_type.name, method_name);
            FunctionSignature *sig = sema_lookup_function(analyzer, method_key);

            // If not found directly, check embedded structs for promoted methods
            if (sig == NULL) {
                StructDefinition *sdef = sema_lookup_struct(analyzer, obj_type->struct_type.name);
                if (sdef != NULL) {
                    for (int32_t ei = 0; ei < BUFFER_LENGTH(sdef->embedded); ei++) {
                        const char *embed_key = arena_sprintf(analyzer->arena, "%s.%s",
                                                              sdef->embedded[ei], method_name);
                        sig = sema_lookup_function(analyzer, embed_key);
                        if (sig != NULL) {
                            break;
                        }
                    }
                }
            }

            if (sig != NULL) {
                // Check method arguments (excluding receiver)
                for (int32_t i = 0; i < BUFFER_LENGTH(node->call.arguments); i++) {
                    check_node(analyzer, node->call.arguments[i]);
                }
                int32_t arg_count = BUFFER_LENGTH(node->call.arguments);

                // Handle named arguments: reorder to match parameter positions
                if (node->call.arg_names != NULL && arg_count > 0) {
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
                                SEMA_ERROR(analyzer, node->call.arguments[i]->location,
                                           "no parameter named '%s'", aname);
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

                if (arg_count != sig->parameter_count) {
                    SEMA_ERROR(analyzer, node->location, "expected %d arguments, got %d",
                               sig->parameter_count, arg_count);
                } else {
                    for (int32_t i = 0; i < arg_count; i++) {
                        if (node->call.arguments[i] == NULL) {
                            continue;
                        }
                        promote_literal(node->call.arguments[i], sig->parameter_types[i]);
                        const Type *at = node->call.arguments[i]->type;
                        if (at != NULL && sig->parameter_types[i] != NULL &&
                            !type_equal(at, sig->parameter_types[i]) && at->kind != TYPE_ERROR &&
                            sig->parameter_types[i]->kind != TYPE_ERROR) {
                            SEMA_ERROR(analyzer, node->call.arguments[i]->location,
                                       "type mismatch: expected '%s', got '%s'",
                                       type_name(analyzer->arena, sig->parameter_types[i]),
                                       type_name(analyzer->arena, at));
                        }
                    }
                }
                node->type = sig->return_type;
                return sig->return_type;
            }
        }
        function_name = method_name;
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
            int32_t arg_count = BUFFER_LENGTH(node->call.arguments);

            // Handle named arguments: reorder to match parameter positions
            if (node->call.arg_names != NULL && arg_count > 0) {
                ASTNode **reordered = NULL;
                for (int32_t i = 0; i < signature->parameter_count; i++) {
                    BUFFER_PUSH(reordered, (ASTNode *)NULL);
                }
                for (int32_t i = 0; i < arg_count; i++) {
                    const char *aname = node->call.arg_names[i];
                    if (aname != NULL) {
                        int32_t idx = -1;
                        for (int32_t j = 0; j < signature->parameter_count; j++) {
                            if (strcmp(signature->parameter_names[j], aname) == 0) {
                                idx = j;
                                break;
                            }
                        }
                        if (idx < 0) {
                            SEMA_ERROR(analyzer, node->call.arguments[i]->location,
                                       "no parameter named '%s'", aname);
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

            if (arg_count != signature->parameter_count) {
                SEMA_ERROR(analyzer, node->location, "expected %d arguments, got %d",
                           signature->parameter_count, arg_count);
            } else {
                for (int32_t i = 0; i < arg_count; i++) {
                    ASTNode *arg = node->call.arguments[i];
                    const Type *param_type = signature->parameter_types[i];
                    promote_literal(arg, param_type);
                    const Type *arg_type = arg->type;
                    if (arg_type != NULL && param_type != NULL &&
                        !type_equal(arg_type, param_type) && arg_type->kind != TYPE_ERROR &&
                        param_type->kind != TYPE_ERROR) {
                        SEMA_ERROR(analyzer, arg->location,
                                   "type mismatch: expected '%s', got '%s'",
                                   type_name(analyzer->arena, param_type),
                                   type_name(analyzer->arena, arg_type));
                    }
                }
            }
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
                promote_literal(fvalue, sdef->fields[j].type);
                const Type *ftype = fvalue->type;
                if (ftype != NULL && sdef->fields[j].type != NULL &&
                    !type_equal(ftype, sdef->fields[j].type) && ftype->kind != TYPE_ERROR &&
                    sdef->fields[j].type->kind != TYPE_ERROR) {
                    SEMA_ERROR(analyzer, fvalue->location, "type mismatch: expected '%s', got '%s'",
                               type_name(analyzer->arena, sdef->fields[j].type),
                               type_name(analyzer->arena, ftype));
                }
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
