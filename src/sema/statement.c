#include "sema/_sema.h"

// ── Reserved-prefix validation ─────────────────────────────────────────

/**
 * Return true (and emit a diagnostic) if @p name starts with a prefix
 * reserved for the compiler or runtime.  Checked prefixes:
 *
 *   rsg_   – runtime functions / types
 *   rsgu_  – codegen-mangled user functions
 *   _rsg   – codegen temporaries and internal variables
 *   _Rsg   – codegen compound-type names
 */
static bool is_reserved_identifier(SemanticAnalyzer *analyzer, SourceLocation location,
                                   const char *name) {
    bool reserved = strncmp(name, "rsg_", 4) == 0 || strncmp(name, "rsgu_", 5) == 0 ||
                    strncmp(name, "_rsg", 4) == 0 || strncmp(name, "_Rsg", 4) == 0;
    if (reserved) {
        SEMA_ERROR(analyzer, location,
                   "identifier '%s' uses a prefix reserved for the compiler/runtime", name);
    }
    return reserved;
}

// ── Statement / declaration checkers ───────────────────────────────────

const Type *check_if(SemanticAnalyzer *analyzer, ASTNode *node) {
    check_node(analyzer, node->if_expression.condition);
    const Type *then_type = check_node(analyzer, node->if_expression.then_body);
    const Type *else_type = NULL;
    if (node->if_expression.else_body != NULL) {
        else_type = check_node(analyzer, node->if_expression.else_body);
    }

    // If both branches present and non-unit, return their common type
    if (else_type != NULL && then_type != NULL && then_type->kind != TYPE_UNIT) {
        return then_type;
    }
    if (else_type != NULL && else_type->kind != TYPE_UNIT) {
        return else_type;
    }
    if (then_type != NULL) {
        return then_type;
    }
    return &TYPE_UNIT_INSTANCE;
}

const Type *check_block(SemanticAnalyzer *analyzer, ASTNode *node) {
    scope_push(analyzer, false);
    for (int32_t i = 0; i < BUFFER_LENGTH(node->block.statements); i++) {
        check_node(analyzer, node->block.statements[i]);
    }
    const Type *result_type = &TYPE_UNIT_INSTANCE;
    if (node->block.result != NULL) {
        result_type = check_node(analyzer, node->block.result);
    }
    scope_pop(analyzer);
    return result_type;
}

/** Promote array literal elements to match the declared element type. */
static bool promote_array_elements(ASTNode *init, const Type *declared) {
    for (int32_t i = 0; i < BUFFER_LENGTH(init->array_literal.elements); i++) {
        ASTNode *elem = init->array_literal.elements[i];
        if (promote_literal(elem, declared->array.element) == NULL && elem->type != NULL &&
            !type_equal(elem->type, declared->array.element)) {
            return false;
        }
    }
    return true;
}

const Type *check_variable_declaration(SemanticAnalyzer *analyzer, ASTNode *node) {
    const Type *init_type = NULL;
    if (node->variable_declaration.initializer != NULL) {
        init_type = check_node(analyzer, node->variable_declaration.initializer);
    }

    const Type *declared = resolve_ast_type(analyzer, &node->variable_declaration.type);

    // Determine final type
    const Type *variable_type;
    if (declared != NULL) {
        variable_type = declared;
        // Promote literal initializer to match declared type
        if (init_type != NULL && node->variable_declaration.initializer != NULL) {
            promote_literal(node->variable_declaration.initializer, declared);

            // Promote array literal elements to match declared element type
            ASTNode *init = node->variable_declaration.initializer;
            bool is_array_mismatch =
                init->kind == NODE_ARRAY_LITERAL && declared->kind == TYPE_ARRAY &&
                init_type->kind == TYPE_ARRAY && !type_equal(declared, init_type);
            if (is_array_mismatch) {
                if (promote_array_elements(init, declared)) {
                    init->type = declared;
                    init_type = declared;
                }
            }

            // Re-read init_type after promotion
            init_type = node->variable_declaration.initializer->type;
        }
        // Check for type mismatch between declared and initializer (non-literal)
        if (init_type != NULL && !type_equal(declared, init_type) &&
            init_type->kind != TYPE_ERROR && declared->kind != TYPE_ERROR) {
            SEMA_ERROR(analyzer, node->location, "type mismatch: expected '%s', got '%s'",
                       type_name(analyzer->arena, declared), type_name(analyzer->arena, init_type));
        }
    } else if (init_type != NULL) {
        variable_type = init_type;
    } else {
        SEMA_ERROR(analyzer, node->location, "cannot infer type for '%s'",
                   node->variable_declaration.name);
        variable_type = &TYPE_ERROR_INSTANCE;
    }

    if (scope_lookup_current(analyzer, node->variable_declaration.name) != NULL) {
        SEMA_ERROR(analyzer, node->location, "redefinition of '%s' in the same scope",
                   node->variable_declaration.name);
    } else if (scope_lookup(analyzer, node->variable_declaration.name) != NULL) {
        SEMA_ERROR(analyzer, node->location, "variable '%s' shadows an existing binding",
                   node->variable_declaration.name);
    }

    is_reserved_identifier(analyzer, node->location, node->variable_declaration.name);

    scope_define(analyzer, node->variable_declaration.name, variable_type, false, SYM_VAR);
    return variable_type;
}

void check_function_body(SemanticAnalyzer *analyzer, ASTNode *function_node) {
    is_reserved_identifier(analyzer, function_node->location,
                           function_node->function_declaration.name);

    scope_push(analyzer, false);

    // Register parameters
    for (int32_t i = 0; i < BUFFER_LENGTH(function_node->function_declaration.parameters); i++) {
        ASTNode *parameter = function_node->function_declaration.parameters[i];
        const Type *parameter_type = resolve_ast_type(analyzer, &parameter->parameter.type);
        if (parameter_type == NULL) {
            parameter_type = &TYPE_ERROR_INSTANCE;
        }
        parameter->type = parameter_type;
        is_reserved_identifier(analyzer, parameter->location, parameter->parameter.name);
        scope_define(analyzer, parameter->parameter.name, parameter_type, false, SYM_PARAM);
    }

    // Check body
    if (function_node->function_declaration.body != NULL) {
        const Type *body_type = check_node(analyzer, function_node->function_declaration.body);

        // If return type not declared, infer from body
        const Type *resolved_return =
            resolve_ast_type(analyzer, &function_node->function_declaration.return_type);
        if (resolved_return == NULL) {
            resolved_return = body_type != NULL ? body_type : &TYPE_UNIT_INSTANCE;
        }
        function_node->type = resolved_return;

        // Update the function's symbol type to the resolved return type
        Symbol *symbol = scope_lookup(analyzer, function_node->function_declaration.name);
        if (symbol != NULL) {
            symbol->type = resolved_return;
        }

        // Update function signatures
        FunctionSignature *signature =
            find_function_signature(analyzer, function_node->function_declaration.name);
        if (signature != NULL && signature->return_type->kind == TYPE_UNIT) {
            signature->return_type = resolved_return;
        }
    }

    scope_pop(analyzer);
}

/** Shared logic for simple and compound assignment type-checking. */
static const Type *check_assignment_common(SemanticAnalyzer *analyzer, ASTNode *target,
                                           ASTNode *value) {
    const Type *target_type = check_node(analyzer, target);
    check_node(analyzer, value);
    promote_literal(value, target_type);
    return &TYPE_UNIT_INSTANCE;
}

const Type *check_assign(SemanticAnalyzer *analyzer, ASTNode *node) {
    return check_assignment_common(analyzer, node->assign.target, node->assign.value);
}

const Type *check_compound_assign(SemanticAnalyzer *analyzer, ASTNode *node) {
    return check_assignment_common(analyzer, node->compound_assign.target,
                                   node->compound_assign.value);
}

// ── Node dispatch ──────────────────────────────────────────────────────

const Type *check_node(SemanticAnalyzer *analyzer, ASTNode *node) {
    if (node == NULL) {
        return &TYPE_UNIT_INSTANCE;
    }
    const Type *result = &TYPE_UNIT_INSTANCE;

    switch (node->kind) {
    case NODE_FILE:
        for (int32_t i = 0; i < BUFFER_LENGTH(node->file.declarations); i++) {
            check_node(analyzer, node->file.declarations[i]);
        }
        break;

    case NODE_MODULE:
        analyzer->current_scope->module_name = node->module.name;
        break;

    case NODE_TYPE_ALIAS:
        // Already processed in first pass
        break;

    case NODE_FUNCTION_DECLARATION:
        check_function_body(analyzer, node);
        result = node->type; // preserve type set by check_function_body
        break;

    case NODE_VARIABLE_DECLARATION:
        result = check_variable_declaration(analyzer, node);
        break;

    case NODE_PARAMETER:
        result = resolve_ast_type(analyzer, &node->parameter.type);
        if (result == NULL) {
            result = &TYPE_ERROR_INSTANCE;
        }
        break;

    case NODE_EXPRESSION_STATEMENT:
        check_node(analyzer, node->expression_statement.expression);
        break;

    case NODE_BREAK:
    case NODE_CONTINUE:
        if (!in_loop(analyzer)) {
            SEMA_ERROR(analyzer, node->location, "'%s' outside of loop",
                       node->kind == NODE_BREAK ? "break" : "continue");
        }
        break;

    case NODE_LITERAL:
        result = check_literal(analyzer, node);
        break;

    case NODE_IDENTIFIER:
        result = check_identifier(analyzer, node);
        break;

    case NODE_UNARY:
        result = check_unary(analyzer, node);
        break;

    case NODE_BINARY:
        result = check_binary(analyzer, node);
        break;

    case NODE_ASSIGN:
        result = check_assign(analyzer, node);
        break;

    case NODE_COMPOUND_ASSIGN:
        result = check_compound_assign(analyzer, node);
        break;

    case NODE_CALL:
        result = check_call(analyzer, node);
        break;

    case NODE_MEMBER:
        result = check_member(analyzer, node);
        break;

    case NODE_INDEX:
        result = check_index(analyzer, node);
        break;

    case NODE_IF:
        result = check_if(analyzer, node);
        break;

    case NODE_LOOP:
        scope_push(analyzer, true);
        check_node(analyzer, node->loop.body);
        scope_pop(analyzer);
        break;

    case NODE_FOR: {
        check_node(analyzer, node->for_loop.start);
        check_node(analyzer, node->for_loop.end);
        scope_push(analyzer, true);
        is_reserved_identifier(analyzer, node->location, node->for_loop.variable_name);
        scope_define(analyzer, node->for_loop.variable_name, &TYPE_I32_INSTANCE, false, SYM_VAR);
        check_node(analyzer, node->for_loop.body);
        scope_pop(analyzer);
        break;
    }

    case NODE_BLOCK:
        result = check_block(analyzer, node);
        break;

    case NODE_STRING_INTERPOLATION:
        result = check_string_interpolation(analyzer, node);
        break;

    case NODE_ARRAY_LITERAL:
        result = check_array_literal(analyzer, node);
        break;

    case NODE_TUPLE_LITERAL:
        result = check_tuple_literal(analyzer, node);
        break;

    case NODE_TYPE_CONVERSION:
        result = check_type_conversion(analyzer, node);
        break;
    }

    node->type = result;
    return result;
}
