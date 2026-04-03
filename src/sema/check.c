#include "sema/_sema.h"

// ── Global tables ──────────────────────────────────────────────────────

TypeAlias *g_type_aliases = NULL;                /* buf */
FunctionSignature *g_function_signatures = NULL; /* buf */

// ── Public API ─────────────────────────────────────────────────────────

SemanticAnalyzer *semantic_analyzer_create(Arena *arena) {
    SemanticAnalyzer *analyzer = rsg_malloc(sizeof(*analyzer));
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
