#include "semantic_analyzer.h"

/** Symbol table entry - one per declared name in a scope. */
struct Symbol {
    const char *name;
    const Type *type;
    bool is_public;
    bool is_function;
    struct Symbol *next; // intrusive linked list within a Scope
};

/**
 * Lexical scope - a linked list of Symbols with a pointer to the
 * enclosing scope.
 */
struct Scope {
    Symbol *symbols;         // head of the symbol chain (may be NULL)
    struct Scope *parent;    // enclosing scope (NULL for global)
    bool is_loop;            // true inside loop/for bodies (enables break/continue)
    const char *module_name; // propagated from the module declaration
};

/** Type alias entry - registered during the first pass. */
typedef struct TypeAlias {
    const char *name;
    const Type *underlying;
} TypeAlias;

static TypeAlias *g_type_aliases = NULL; /* buf */

struct SemanticAnalyzer {
    Arena *arena;
    Scope *current_scope;
    int32_t error_count;
};

// Scope manipulation - push/pop/define/lookup.

/**
 * Push a new child scope.  If @p is_loop is true, break/continue are legal
 * inside it.
 */
static Scope *scope_push(SemanticAnalyzer *analyzer, bool is_loop) {
    Scope *scope = arena_alloc(analyzer->arena, sizeof(Scope));
    scope->symbols = NULL;
    scope->parent = analyzer->current_scope;
    scope->is_loop = is_loop;
    scope->module_name = analyzer->current_scope != NULL ? analyzer->current_scope->module_name : NULL;
    analyzer->current_scope = scope;
    return scope;
}

static void scope_pop(SemanticAnalyzer *analyzer) {
    analyzer->current_scope = analyzer->current_scope->parent;
}

/** Define @p name in the current scope. */
static void scope_define(SemanticAnalyzer *analyzer, const char *name, const Type *type, bool is_public,
                         bool is_function) {
    Symbol *symbol = arena_alloc(analyzer->arena, sizeof(Symbol));
    symbol->name = name;
    symbol->type = type;
    symbol->is_public = is_public;
    symbol->is_function = is_function;
    symbol->next = analyzer->current_scope->symbols;
    analyzer->current_scope->symbols = symbol;
}

/** Look up @p name in the innermost scope only (for redefinition checks). */
static Symbol *scope_lookup_current(const SemanticAnalyzer *analyzer, const char *name) {
    for (Symbol *symbol = analyzer->current_scope->symbols; symbol != NULL; symbol = symbol->next) {
        if (strcmp(symbol->name, name) == 0) {
            return symbol;
        }
    }
    return NULL;
}

/** Walk the scope chain outward to find @p name. */
static Symbol *scope_lookup(const SemanticAnalyzer *analyzer, const char *name) {
    for (Scope *scope = analyzer->current_scope; scope != NULL; scope = scope->parent) {
        for (Symbol *symbol = scope->symbols; symbol != NULL; symbol = symbol->next) {
            if (strcmp(symbol->name, name) == 0) {
                return symbol;
            }
        }
    }
    return NULL;
}

/** Return true if any enclosing scope has is_loop set. */
static bool in_loop(const SemanticAnalyzer *analyzer) {
    for (Scope *scope = analyzer->current_scope; scope != NULL; scope = scope->parent) {
        if (scope->is_loop) {
            return true;
        }
    }
    return false;
}

/** Look up a type alias by name. Returns the underlying type or NULL. */
static const Type *find_type_alias(const char *name) {
    for (int32_t i = 0; i < BUFFER_LENGTH(g_type_aliases); i++) {
        if (strcmp(g_type_aliases[i].name, name) == 0) {
            return g_type_aliases[i].underlying;
        }
    }
    return NULL;
}

/**
 * Map a syntactic ASTType to a resolved Type*.  Returns NULL for inferred
 * types; emits an error and returns TYPE_ERROR for unknown names.
 */
static const Type *resolve_ast_type(SemanticAnalyzer *analyzer, const ASTType *ast_type) {
    if (ast_type == NULL || ast_type->kind == AST_TYPE_INFERRED) {
        return NULL;
    }
    if (ast_type->kind == AST_TYPE_ARRAY) {
        const Type *element = resolve_ast_type(analyzer, ast_type->array_element);
        if (element == NULL) {
            rsg_error(ast_type->location, "array element type required");
            analyzer->error_count++;
            return &TYPE_ERROR_INSTANCE;
        }
        return type_create_array(analyzer->arena, element, ast_type->array_size);
    }
    if (ast_type->kind == AST_TYPE_TUPLE) {
        const Type **elements = NULL;
        for (int32_t i = 0; i < BUFFER_LENGTH(ast_type->tuple_elements); i++) {
            const Type *element = resolve_ast_type(analyzer, ast_type->tuple_elements[i]);
            if (element == NULL) {
                element = &TYPE_ERROR_INSTANCE;
            }
            BUFFER_PUSH(elements, element);
        }
        return type_create_tuple(analyzer->arena, elements, BUFFER_LENGTH(elements));
    }
    // AST_TYPE_NAME
    const Type *type = type_from_name(ast_type->name);
    if (type != NULL) {
        return type;
    }
    // Check type aliases
    const Type *alias = find_type_alias(ast_type->name);
    if (alias != NULL) {
        return alias;
    }
    rsg_error(ast_type->location, "unknown type '%s'", ast_type->name);
    analyzer->error_count++;
    return &TYPE_ERROR_INSTANCE;
}

/**
 * Stored function signature - registered in pass 1 so that forward calls
 * can be type-checked in pass 2.
 */
typedef struct FunctionSignature {
    const char *name;
    const Type *return_type;
    const Type **parameter_types; /* buf */
    int32_t parameter_count;
    bool is_public;
} FunctionSignature;

static FunctionSignature *g_function_signatures = NULL; /* buf */

static FunctionSignature *find_function_signature(const char *name) {
    for (int32_t i = 0; i < BUFFER_LENGTH(g_function_signatures); i++) {
        if (strcmp(g_function_signatures[i].name, name) == 0) {
            return &g_function_signatures[i];
        }
    }
    return NULL;
}

/** Recursive AST walk - type-checks each node and returns its resolved type. */
static const Type *check_node(SemanticAnalyzer *analyzer, ASTNode *node);

/** Map a literal kind to its corresponding integer type. */
static const Type *literal_kind_to_type(LiteralKind kind) {
    switch (kind) {
    case LITERAL_BOOL:
        return &TYPE_BOOL_INSTANCE;
    case LITERAL_I8:
        return &TYPE_I8_INSTANCE;
    case LITERAL_I16:
        return &TYPE_I16_INSTANCE;
    case LITERAL_I32:
        return &TYPE_I32_INSTANCE;
    case LITERAL_I64:
        return &TYPE_I64_INSTANCE;
    case LITERAL_I128:
        return &TYPE_I128_INSTANCE;
    case LITERAL_U8:
        return &TYPE_U8_INSTANCE;
    case LITERAL_U16:
        return &TYPE_U16_INSTANCE;
    case LITERAL_U32:
        return &TYPE_U32_INSTANCE;
    case LITERAL_U64:
        return &TYPE_U64_INSTANCE;
    case LITERAL_U128:
        return &TYPE_U128_INSTANCE;
    case LITERAL_ISIZE:
        return &TYPE_ISIZE_INSTANCE;
    case LITERAL_USIZE:
        return &TYPE_USIZE_INSTANCE;
    case LITERAL_F32:
        return &TYPE_F32_INSTANCE;
    case LITERAL_F64:
        return &TYPE_F64_INSTANCE;
    case LITERAL_CHAR:
        return &TYPE_CHAR_INSTANCE;
    case LITERAL_STRING:
        return &TYPE_STRING_INSTANCE;
    case LITERAL_UNIT:
        return &TYPE_UNIT_INSTANCE;
    }
    return &TYPE_ERROR_INSTANCE;
}

/** Return the LiteralKind for a given TypeKind. */
static LiteralKind type_to_literal_kind(TypeKind kind) {
    switch (kind) {
    case TYPE_BOOL:
        return LITERAL_BOOL;
    case TYPE_I8:
        return LITERAL_I8;
    case TYPE_I16:
        return LITERAL_I16;
    case TYPE_I32:
        return LITERAL_I32;
    case TYPE_I64:
        return LITERAL_I64;
    case TYPE_I128:
        return LITERAL_I128;
    case TYPE_U8:
        return LITERAL_U8;
    case TYPE_U16:
        return LITERAL_U16;
    case TYPE_U32:
        return LITERAL_U32;
    case TYPE_U64:
        return LITERAL_U64;
    case TYPE_U128:
        return LITERAL_U128;
    case TYPE_ISIZE:
        return LITERAL_ISIZE;
    case TYPE_USIZE:
        return LITERAL_USIZE;
    case TYPE_F32:
        return LITERAL_F32;
    case TYPE_F64:
        return LITERAL_F64;
    case TYPE_CHAR:
        return LITERAL_CHAR;
    case TYPE_STRING:
        return LITERAL_STRING;
    case TYPE_UNIT:
        return LITERAL_UNIT;
    default:
        return LITERAL_I32;
    }
}

static const Type *check_literal(SemanticAnalyzer *analyzer, ASTNode *node) {
    (void)analyzer;
    return literal_kind_to_type(node->literal.kind);
}

static const Type *check_identifier(SemanticAnalyzer *analyzer, ASTNode *node) {
    Symbol *symbol = scope_lookup(analyzer, node->identifier.name);
    if (symbol == NULL) {
        rsg_error(node->location, "undefined variable '%s'", node->identifier.name);
        analyzer->error_count++;
        return &TYPE_ERROR_INSTANCE;
    }
    return symbol->type;
}

static const Type *check_unary(SemanticAnalyzer *analyzer, ASTNode *node) {
    const Type *operand = check_node(analyzer, node->unary.operand);
    if (node->unary.op == TOKEN_BANG) {
        return &TYPE_BOOL_INSTANCE;
    }
    if (node->unary.op == TOKEN_MINUS) {
        return operand;
    }
    return operand;
}

/**
 * Promote a literal node to match @p target's numeric type.
 * Integer literals (i32 default) can be promoted to any integer or float type.
 * Float literals (f64 default) can be promoted to f32.
 * Returns @p target on success, NULL if no promotion applies.
 */
static const Type *promote_literal(ASTNode *literal, const Type *target) {
    if (literal == NULL || target == NULL) {
        return NULL;
    }

    // Handle negated literal: -(literal)
    if (literal->kind == NODE_UNARY && literal->unary.op == TOKEN_MINUS &&
        literal->unary.operand->kind == NODE_LITERAL) {
        const Type *result = promote_literal(literal->unary.operand, target);
        if (result != NULL) {
            literal->type = result;
            return result;
        }
        return NULL;
    }

    if (literal->kind != NODE_LITERAL) {
        return NULL;
    }

    // Promote integer literal (i32 default) to any integer type
    if (literal->literal.kind == LITERAL_I32 && type_is_integer(target)) {
        literal->literal.kind = type_to_literal_kind(target->kind);
        literal->type = target;
        return target;
    }

    // Promote integer literal to float type
    if (literal->literal.kind == LITERAL_I32 && type_is_float(target)) {
        literal->literal.kind = (target->kind == TYPE_F32) ? LITERAL_F32 : LITERAL_F64;
        literal->literal.float64_value = (double)literal->literal.integer_value;
        literal->type = target;
        return target;
    }

    // Promote u32 literal to larger unsigned types
    if (literal->literal.kind == LITERAL_U32 && type_is_unsigned_integer(target)) {
        literal->literal.kind = type_to_literal_kind(target->kind);
        literal->type = target;
        return target;
    }

    // Promote f64 literal to f32
    if (literal->literal.kind == LITERAL_F64 && target->kind == TYPE_F32) {
        literal->literal.kind = LITERAL_F32;
        literal->type = target;
        return target;
    }

    return NULL;
}

static const Type *check_binary(SemanticAnalyzer *analyzer, ASTNode *node) {
    const Type *left = check_node(analyzer, node->binary.left);
    const Type *right = check_node(analyzer, node->binary.right);

    if (left == NULL || right == NULL || left->kind == TYPE_ERROR || right->kind == TYPE_ERROR) {
        return (node->binary.op >= TOKEN_EQUAL_EQUAL && node->binary.op <= TOKEN_GREATER_EQUAL) ||
                       node->binary.op == TOKEN_AMPERSAND_AMPERSAND || node->binary.op == TOKEN_PIPE_PIPE
                   ? &TYPE_BOOL_INSTANCE
                   : &TYPE_ERROR_INSTANCE;
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
        // Only report for non-error types
        if (left->kind != TYPE_ERROR && right->kind != TYPE_ERROR) {
            rsg_error(node->location, "type mismatch: '%s' and '%s'", type_name(left), type_name(right));
            analyzer->error_count++;
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

static const Type *check_call(SemanticAnalyzer *analyzer, ASTNode *node) {
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
        FunctionSignature *signature = find_function_signature(function_name);
        if (signature != NULL) {
            // Check argument count and types
            int32_t arg_count = BUFFER_LENGTH(node->call.arguments);
            if (arg_count != signature->parameter_count) {
                rsg_error(node->location, "expected %d arguments, got %d", signature->parameter_count, arg_count);
                analyzer->error_count++;
            } else {
                for (int32_t i = 0; i < arg_count; i++) {
                    ASTNode *arg = node->call.arguments[i];
                    const Type *param_type = signature->parameter_types[i];
                    // Promote literal arguments to match parameter type
                    promote_literal(arg, param_type);
                    const Type *arg_type = arg->type;
                    if (arg_type != NULL && param_type != NULL && !type_equal(arg_type, param_type) &&
                        arg_type->kind != TYPE_ERROR && param_type->kind != TYPE_ERROR) {
                        rsg_error(arg->location, "type mismatch: expected '%s', got '%s'", type_name(param_type),
                                  type_name(arg_type));
                        analyzer->error_count++;
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
            rsg_error(node->location, "undefined function '%s'", function_name);
            analyzer->error_count++;
        }
    }
    return &TYPE_ERROR_INSTANCE;
}

static const Type *check_if(SemanticAnalyzer *analyzer, ASTNode *node) {
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

static const Type *check_block(SemanticAnalyzer *analyzer, ASTNode *node) {
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

static const Type *check_variable_declaration(SemanticAnalyzer *analyzer, ASTNode *node) {
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
            if (init->kind == NODE_ARRAY_LITERAL && declared->kind == TYPE_ARRAY && init_type->kind == TYPE_ARRAY &&
                !type_equal(declared, init_type)) {
                bool promoted = true;
                for (int32_t i = 0; i < BUFFER_LENGTH(init->array_literal.elements); i++) {
                    if (promote_literal(init->array_literal.elements[i], declared->array_element) == NULL &&
                        init->array_literal.elements[i]->type != NULL &&
                        !type_equal(init->array_literal.elements[i]->type, declared->array_element)) {
                        promoted = false;
                        break;
                    }
                }
                if (promoted) {
                    init->type = declared;
                    init_type = declared;
                }
            }

            // Re-read init_type after promotion
            init_type = node->variable_declaration.initializer->type;
        }
        // Check for type mismatch between declared and initializer (non-literal)
        if (init_type != NULL && !type_equal(declared, init_type) && init_type->kind != TYPE_ERROR &&
            declared->kind != TYPE_ERROR) {
            rsg_error(node->location, "type mismatch: expected '%s', got '%s'", type_name(declared),
                      type_name(init_type));
            analyzer->error_count++;
        }
    } else if (init_type != NULL) {
        variable_type = init_type;
    } else {
        rsg_error(node->location, "cannot infer type for '%s'", node->variable_declaration.name);
        analyzer->error_count++;
        variable_type = &TYPE_ERROR_INSTANCE;
    }

    if (scope_lookup_current(analyzer, node->variable_declaration.name) != NULL) {
        rsg_error(node->location, "redefinition of '%s' in the same scope", node->variable_declaration.name);
        analyzer->error_count++;
    } else if (scope_lookup(analyzer, node->variable_declaration.name) != NULL) {
        rsg_error(node->location, "variable '%s' shadows an existing binding", node->variable_declaration.name);
        analyzer->error_count++;
    }

    scope_define(analyzer, node->variable_declaration.name, variable_type, false, false);
    return variable_type;
}

static void check_function_body(SemanticAnalyzer *analyzer, ASTNode *function_node) {
    scope_push(analyzer, false);

    // Register parameters
    for (int32_t i = 0; i < BUFFER_LENGTH(function_node->function_declaration.parameters); i++) {
        ASTNode *parameter = function_node->function_declaration.parameters[i];
        const Type *parameter_type = resolve_ast_type(analyzer, &parameter->parameter.type);
        if (parameter_type == NULL) {
            parameter_type = &TYPE_ERROR_INSTANCE;
        }
        parameter->type = parameter_type;
        scope_define(analyzer, parameter->parameter.name, parameter_type, false, false);
    }

    // Check body
    if (function_node->function_declaration.body != NULL) {
        const Type *body_type = check_node(analyzer, function_node->function_declaration.body);

        // If return type not declared, infer from body
        const Type *resolved_return = resolve_ast_type(analyzer, &function_node->function_declaration.return_type);
        if (resolved_return == NULL) {
            resolved_return = body_type != NULL ? body_type : &TYPE_UNIT_INSTANCE;
        }
        function_node->type = resolved_return;

        // Update the function's symbol type to the resolved return type
        Symbol *symbol = scope_lookup(analyzer, function_node->function_declaration.name);
        if (symbol != NULL) {
            symbol->type = resolved_return;
        }

        // Update g_function_signatures
        FunctionSignature *signature = find_function_signature(function_node->function_declaration.name);
        if (signature != NULL && signature->return_type->kind == TYPE_UNIT) {
            signature->return_type = resolved_return;
        }
    }

    scope_pop(analyzer);
}

static const Type *check_assign(SemanticAnalyzer *analyzer, ASTNode *node) {
    const Type *target_type = check_node(analyzer, node->assign.target);
    check_node(analyzer, node->assign.value);
    // Promote literal value to match target type
    promote_literal(node->assign.value, target_type);
    return &TYPE_UNIT_INSTANCE;
}

static const Type *check_compound_assign(SemanticAnalyzer *analyzer, ASTNode *node) {
    const Type *target_type = check_node(analyzer, node->compound_assign.target);
    check_node(analyzer, node->compound_assign.value);
    promote_literal(node->compound_assign.value, target_type);
    return &TYPE_UNIT_INSTANCE;
}

static const Type *check_string_interpolation(SemanticAnalyzer *analyzer, ASTNode *node) {
    for (int32_t i = 0; i < BUFFER_LENGTH(node->string_interpolation.parts); i++) {
        check_node(analyzer, node->string_interpolation.parts[i]);
    }
    return &TYPE_STRING_INSTANCE;
}

static const Type *check_array_literal(SemanticAnalyzer *analyzer, ASTNode *node) {
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

static const Type *check_tuple_literal(SemanticAnalyzer *analyzer, ASTNode *node) {
    const Type **element_types = NULL;
    for (int32_t i = 0; i < BUFFER_LENGTH(node->tuple_literal.elements); i++) {
        const Type *elem = check_node(analyzer, node->tuple_literal.elements[i]);
        BUFFER_PUSH(element_types, elem);
    }
    return type_create_tuple(analyzer->arena, element_types, BUFFER_LENGTH(element_types));
}

static const Type *check_index(SemanticAnalyzer *analyzer, ASTNode *node) {
    const Type *object_type = check_node(analyzer, node->index_access.object);
    check_node(analyzer, node->index_access.index);
    if (object_type != NULL && object_type->kind == TYPE_ARRAY) {
        return object_type->array_element;
    }
    return &TYPE_ERROR_INSTANCE;
}

static const Type *check_member(SemanticAnalyzer *analyzer, ASTNode *node) {
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

static const Type *check_type_conversion(SemanticAnalyzer *analyzer, ASTNode *node) {
    check_node(analyzer, node->type_conversion.operand);
    const Type *target = resolve_ast_type(analyzer, &node->type_conversion.target_type);
    if (target == NULL) {
        return &TYPE_ERROR_INSTANCE;
    }
    // Promote the operand literal
    promote_literal(node->type_conversion.operand, target);
    return target;
}

static const Type *check_node(SemanticAnalyzer *analyzer, ASTNode *node) {
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
            rsg_error(node->location, "'%s' outside of loop", node->kind == NODE_BREAK ? "break" : "continue");
            analyzer->error_count++;
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
        scope_define(analyzer, node->for_loop.variable_name, &TYPE_I32_INSTANCE, false, false);
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

SemanticAnalyzer *semantic_analyzer_create(Arena *arena) {
    SemanticAnalyzer *analyzer = malloc(sizeof(*analyzer));
    if (analyzer == NULL) {
        rsg_fatal("out of memory");
    }
    analyzer->arena = arena;
    analyzer->current_scope = NULL;
    analyzer->error_count = 0;
    return analyzer;
}

void semantic_analyzer_destroy(SemanticAnalyzer *analyzer) {
    free(analyzer);
}

bool semantic_analyzer_check(SemanticAnalyzer *analyzer, ASTNode *file) {
    // Reset globals for each compilation
    if (g_function_signatures != NULL) {
        BUFFER_FREE(g_function_signatures);
        g_function_signatures = NULL;
    }
    if (g_type_aliases != NULL) {
        BUFFER_FREE(g_type_aliases);
        g_type_aliases = NULL;
    }

    scope_push(analyzer, false); // global scope

    // First pass: register type aliases and function signatures
    for (int32_t i = 0; i < BUFFER_LENGTH(file->file.declarations); i++) {
        ASTNode *declaration = file->file.declarations[i];

        if (declaration->kind == NODE_TYPE_ALIAS) {
            const Type *underlying = resolve_ast_type(analyzer, &declaration->type_alias.alias_type);
            if (underlying != NULL) {
                TypeAlias alias = {.name = declaration->type_alias.name, .underlying = underlying};
                BUFFER_PUSH(g_type_aliases, alias);
            }
        }

        if (declaration->kind == NODE_FUNCTION_DECLARATION) {
            // Resolve return type (may be inferred - default to unit)
            const Type *resolved_return = &TYPE_UNIT_INSTANCE;
            if (declaration->function_declaration.return_type.kind != AST_TYPE_INFERRED) {
                resolved_return = resolve_ast_type(analyzer, &declaration->function_declaration.return_type);
                if (resolved_return == NULL) {
                    resolved_return = &TYPE_UNIT_INSTANCE;
                }
            }

            // Build parameter types
            FunctionSignature signature;
            signature.name = declaration->function_declaration.name;
            signature.return_type = resolved_return;
            signature.parameter_types = NULL;
            signature.parameter_count = BUFFER_LENGTH(declaration->function_declaration.parameters);
            signature.is_public = declaration->function_declaration.is_public;
            for (int32_t j = 0; j < signature.parameter_count; j++) {
                ASTNode *parameter = declaration->function_declaration.parameters[j];
                const Type *parameter_type = resolve_ast_type(analyzer, &parameter->parameter.type);
                if (parameter_type == NULL) {
                    parameter_type = &TYPE_ERROR_INSTANCE;
                }
                BUFFER_PUSH(signature.parameter_types, parameter_type);
            }
            BUFFER_PUSH(g_function_signatures, signature);

            // Register in scope
            scope_define(analyzer, declaration->function_declaration.name, resolved_return,
                         declaration->function_declaration.is_public, true);
        }
    }

    // Second pass: type-check everything
    check_node(analyzer, file);

    scope_pop(analyzer);
    return analyzer->error_count == 0;
}
