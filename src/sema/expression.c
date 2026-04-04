#include "sema/_sema.h"

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
        bool is_boolean_result =
            (node->binary.op >= TOKEN_EQUAL_EQUAL && node->binary.op <= TOKEN_GREATER_EQUAL) ||
            node->binary.op == TOKEN_AMPERSAND_AMPERSAND || node->binary.op == TOKEN_PIPE_PIPE;
        return is_boolean_result ? &TYPE_BOOL_INSTANCE : &TYPE_ERROR_INSTANCE;
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
        function_name = node->call.callee->member.member;
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
        FunctionSignature *signature = find_function_signature(analyzer, function_name);
        if (signature != NULL) {
            int32_t arg_count = BUFFER_LENGTH(node->call.arguments);
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
        if (symbol != NULL && symbol->is_function) {
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
        if (end != NULL && *end == '\0' && index >= 0 && index < object_type->tuple_count) {
            return object_type->tuple_elements[index];
        }
    }
    return &TYPE_ERROR_INSTANCE;
}

const Type *check_index(SemanticAnalyzer *analyzer, ASTNode *node) {
    const Type *object_type = check_node(analyzer, node->index_access.object);
    check_node(analyzer, node->index_access.index);
    if (object_type != NULL && object_type->kind == TYPE_ARRAY) {
        return object_type->array_element;
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
