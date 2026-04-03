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

/**
 * Map a syntactic ASTType to a resolved Type*.  Returns NULL for inferred
 * types; emits an error and returns TYPE_ERROR for unknown names.
 */
static const Type *resolve_ast_type(SemanticAnalyzer *analyzer, const ASTType *ast_type) {
    if (ast_type == NULL || ast_type->kind == AST_TYPE_INFERRED) {
        return NULL;
    }
    const Type *type = type_from_name(ast_type->name);
    if (type == NULL) {
        rsg_error(ast_type->location, "unknown type '%s'", ast_type->name);
        analyzer->error_count++;
        return &TYPE_ERROR_INSTANCE;
    }
    return type;
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

static const Type *check_literal(SemanticAnalyzer *analyzer, ASTNode *node) {
    (void)analyzer;
    switch (node->literal.kind) {
    case LITERAL_BOOL:
        return &TYPE_BOOL_INSTANCE;
    case LITERAL_I32:
        return &TYPE_I32_INSTANCE;
    case LITERAL_U32:
        return &TYPE_U32_INSTANCE;
    case LITERAL_F64:
        return &TYPE_F64_INSTANCE;
    case LITERAL_STRING:
        return &TYPE_STRING_INSTANCE;
    case LITERAL_UNIT:
        return &TYPE_UNIT_INSTANCE;
    }
    return &TYPE_ERROR_INSTANCE;
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
    if (node->unary.operator== TOKEN_BANG) {
        return &TYPE_BOOL_INSTANCE;
    }
    if (node->unary.operator== TOKEN_MINUS) {
        return operand;
    }
    return operand;
}

/**
 * Promote an integer literal node to match @p target's numeric type
 * (u32 or f64).  Returns @p target on success, NULL if no promotion
 * applies.
 */
static const Type *promote_literal(ASTNode *literal, const Type *target) {
    if (literal == NULL || literal->kind != NODE_LITERAL) {
        return NULL;
    }
    if (literal->literal.kind != LITERAL_I32 && literal->literal.kind != LITERAL_U32) {
        return NULL;
    }
    if (target->kind == TYPE_U32) {
        literal->literal.kind = LITERAL_U32;
    } else if (target->kind == TYPE_F64) {
        literal->literal.kind = LITERAL_F64;
        literal->literal.float64_value = (double)literal->literal.integer_value;
    } else {
        return NULL;
    }
    literal->type = target;
    return target;
}

static const Type *check_binary(SemanticAnalyzer *analyzer, ASTNode *node) {
    const Type *left = check_node(analyzer, node->binary.left);
    const Type *right = check_node(analyzer, node->binary.right);

    // Promote integer literal to match the other side's numeric type
    if (left != NULL && right != NULL) {
        if (type_is_numeric(left) && type_is_numeric(right) && !type_equal(left, right)) {
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
    }

    switch (node->binary.operator) {
    // Comparison and logical operators return bool
    case TOKEN_EQUAL_EQUAL:
    case TOKEN_BANG_EQUAL:
    case TOKEN_LESS:
    case TOKEN_LESS_EQUAL:
    case TOKEN_GREATER:
    case TOKEN_GREATER_EQUAL:
    case TOKEN_AMPERSAND_AMPERSAND:
    case TOKEN_PIPE_PIPE:
        return &TYPE_BOOL_INSTANCE;

    // Arithmetic returns the operand type
    default:
        return left != NULL ? left : right;
    }
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

    // Look up function return type
    if (function_name != NULL) {
        Symbol *symbol = scope_lookup(analyzer, function_name);
        if (symbol != NULL && symbol->is_function) {
            return symbol->type;
        }
        // Try g_function_signatures
        FunctionSignature *signature = find_function_signature(function_name);
        if (signature != NULL) {
            return signature->return_type;
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
        // If init is a literal, retype it to match declared type
        if (init_type != NULL && node->variable_declaration.initializer != NULL &&
            node->variable_declaration.initializer->kind == NODE_LITERAL) {
            if (declared->kind == TYPE_U32 && node->variable_declaration.initializer->literal.kind == LITERAL_I32) {
                node->variable_declaration.initializer->literal.kind = LITERAL_U32;
                node->variable_declaration.initializer->type = declared;
            }
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
    check_node(analyzer, node->assign.target);
    check_node(analyzer, node->assign.value);
    return &TYPE_UNIT_INSTANCE;
}

static const Type *check_compound_assign(SemanticAnalyzer *analyzer, ASTNode *node) {
    check_node(analyzer, node->compound_assign.target);
    check_node(analyzer, node->compound_assign.value);
    return &TYPE_UNIT_INSTANCE;
}

static const Type *check_string_interpolation(SemanticAnalyzer *analyzer, ASTNode *node) {
    for (int32_t i = 0; i < BUFFER_LENGTH(node->string_interpolation.parts); i++) {
        check_node(analyzer, node->string_interpolation.parts[i]);
    }
    return &TYPE_STRING_INSTANCE;
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
        check_node(analyzer, node->member.object);
        result = &TYPE_ERROR_INSTANCE;
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
    // Reset g_function_signatures for each compilation
    if (g_function_signatures != NULL) {
        BUFFER_FREE(g_function_signatures);
        g_function_signatures = NULL;
    }

    scope_push(analyzer, false); // global scope

    // First pass: register all function signatures (forward declarations)
    for (int32_t i = 0; i < BUFFER_LENGTH(file->file.declarations); i++) {
        ASTNode *declaration = file->file.declarations[i];
        if (declaration->kind == NODE_FUNCTION_DECLARATION) {
            // Resolve return type (may be inferred - default to unit)
            const Type *resolved_return = &TYPE_UNIT_INSTANCE;
            if (declaration->function_declaration.return_type.kind == AST_TYPE_NAME) {
                resolved_return = type_from_name(declaration->function_declaration.return_type.name);
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
                const Type *parameter_type = type_from_name(parameter->parameter.type.name);
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
