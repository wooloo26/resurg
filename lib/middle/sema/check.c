#include "_sema.h"

// ── Public API ─────────────────────────────────────────────────────────

SemanticAnalyzer *semantic_analyzer_create(Arena *arena) {
    SemanticAnalyzer *analyzer = rsg_malloc(sizeof(*analyzer));
    analyzer->arena = arena;
    analyzer->current_scope = NULL;
    analyzer->error_count = 0;
    hash_table_init(&analyzer->type_alias_table, NULL);
    hash_table_init(&analyzer->function_table, NULL);
    return analyzer;
}

void semantic_analyzer_destroy(SemanticAnalyzer *analyzer) {
    if (analyzer != NULL) {
        hash_table_destroy(&analyzer->type_alias_table);
        hash_table_destroy(&analyzer->function_table);
        free(analyzer);
    }
}

bool semantic_analyzer_check(SemanticAnalyzer *analyzer, ASTNode *file) {
    // Reset tables for each compilation
    hash_table_destroy(&analyzer->function_table);
    hash_table_init(&analyzer->function_table, NULL);
    hash_table_destroy(&analyzer->type_alias_table);
    hash_table_init(&analyzer->type_alias_table, NULL);

    scope_push(analyzer, false); // global scope

    // First pass: register type aliases and function signatures
    for (int32_t i = 0; i < BUFFER_LENGTH(file->file.declarations); i++) {
        ASTNode *declaration = file->file.declarations[i];

        if (declaration->kind == NODE_TYPE_ALIAS) {
            const Type *underlying =
                resolve_ast_type(analyzer, &declaration->type_alias.alias_type);
            if (underlying != NULL) {
                hash_table_insert(&analyzer->type_alias_table, declaration->type_alias.name,
                                  (void *)underlying);
            }
        }

        if (declaration->kind == NODE_FUNCTION_DECLARATION) {
            // Resolve return type (may be inferred - default to unit)
            const Type *resolved_return = &TYPE_UNIT_INSTANCE;
            if (declaration->function_declaration.return_type.kind != AST_TYPE_INFERRED) {
                resolved_return =
                    resolve_ast_type(analyzer, &declaration->function_declaration.return_type);
                if (resolved_return == NULL) {
                    resolved_return = &TYPE_UNIT_INSTANCE;
                }
            }

            // Build parameter types
            FunctionSignature *signature = rsg_malloc(sizeof(*signature));
            signature->name = declaration->function_declaration.name;
            signature->return_type = resolved_return;
            signature->parameter_types = NULL;
            signature->parameter_count =
                BUFFER_LENGTH(declaration->function_declaration.parameters);
            signature->is_public = declaration->function_declaration.is_public;
            for (int32_t j = 0; j < signature->parameter_count; j++) {
                ASTNode *parameter = declaration->function_declaration.parameters[j];
                const Type *parameter_type = resolve_ast_type(analyzer, &parameter->parameter.type);
                if (parameter_type == NULL) {
                    parameter_type = &TYPE_ERROR_INSTANCE;
                }
                BUFFER_PUSH(signature->parameter_types, parameter_type);
            }
            hash_table_insert(&analyzer->function_table, declaration->function_declaration.name,
                              signature);

            // Register in scope
            scope_define(analyzer, declaration->function_declaration.name, resolved_return,
                         declaration->function_declaration.is_public, SYM_FUNCTION);
        }
    }

    // Second pass: type-check everything
    check_node(analyzer, file);

    scope_pop(analyzer);
    return analyzer->error_count == 0;
}
