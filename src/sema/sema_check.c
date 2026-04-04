#include "_sema.h"

// ── Public API ─────────────────────────────────────────────────────────

SemanticAnalyzer *semantic_analyzer_create(Arena *arena) {
    SemanticAnalyzer *analyzer = rsg_malloc(sizeof(*analyzer));
    analyzer->arena = arena;
    analyzer->current_scope = NULL;
    analyzer->error_count = 0;
    hash_table_init(&analyzer->type_alias_table, NULL);
    hash_table_init(&analyzer->function_table, NULL);
    hash_table_init(&analyzer->struct_table, NULL);
    return analyzer;
}

void semantic_analyzer_destroy(SemanticAnalyzer *analyzer) {
    if (analyzer != NULL) {
        hash_table_destroy(&analyzer->type_alias_table);
        hash_table_destroy(&analyzer->function_table);
        hash_table_destroy(&analyzer->struct_table);
        free(analyzer);
    }
}

/** Register a single function's signature into the analyzer tables and scope. */
static void register_function_signature(SemanticAnalyzer *analyzer, ASTNode *declaration) {
    const Type *resolved_return = &TYPE_UNIT_INSTANCE;
    if (declaration->function_declaration.return_type.kind != AST_TYPE_INFERRED) {
        resolved_return =
            resolve_ast_type(analyzer, &declaration->function_declaration.return_type);
        if (resolved_return == NULL) {
            resolved_return = &TYPE_UNIT_INSTANCE;
        }
    }

    FunctionSignature *signature = rsg_malloc(sizeof(*signature));
    signature->name = declaration->function_declaration.name;
    signature->return_type = resolved_return;
    signature->parameter_types = NULL;
    signature->parameter_names = NULL;
    signature->parameter_count = BUFFER_LENGTH(declaration->function_declaration.parameters);
    signature->is_public = declaration->function_declaration.is_public;
    for (int32_t j = 0; j < signature->parameter_count; j++) {
        ASTNode *parameter = declaration->function_declaration.parameters[j];
        const Type *parameter_type = resolve_ast_type(analyzer, &parameter->parameter.type);
        if (parameter_type == NULL) {
            parameter_type = &TYPE_ERROR_INSTANCE;
        }
        BUFFER_PUSH(signature->parameter_types, parameter_type);
        BUFFER_PUSH(signature->parameter_names, parameter->parameter.name);
    }
    hash_table_insert(&analyzer->function_table, declaration->function_declaration.name, signature);

    scope_define(analyzer, declaration->function_declaration.name, resolved_return,
                 declaration->function_declaration.is_public, SYM_FUNCTION);
}

/** Register a struct definition: build the TYPE_STRUCT, register methods as functions. */
static void register_struct_definition(SemanticAnalyzer *analyzer, ASTNode *declaration) {
    const char *struct_name = declaration->struct_declaration.name;

    // Check for duplicate struct definition
    if (find_struct_definition(analyzer, struct_name) != NULL) {
        SEMA_ERROR(analyzer, declaration->location, "duplicate struct definition '%s'",
                   struct_name);
        return;
    }

    StructDefinition *def = rsg_malloc(sizeof(*def));
    def->name = struct_name;
    def->fields = NULL;
    def->methods = NULL;
    def->embedded = NULL;

    // Collect embedded struct types
    for (int32_t i = 0; i < BUFFER_LENGTH(declaration->struct_declaration.embedded); i++) {
        BUFFER_PUSH(def->embedded, declaration->struct_declaration.embedded[i]);
    }

    // Build field list: promoted fields from embedded, then own fields
    // Resolve embedded types and promote their fields
    const Type **embedded_types = NULL;
    for (int32_t i = 0; i < BUFFER_LENGTH(def->embedded); i++) {
        StructDefinition *embed_def = find_struct_definition(analyzer, def->embedded[i]);
        if (embed_def == NULL) {
            SEMA_ERROR(analyzer, declaration->location, "unknown embedded struct '%s'",
                       def->embedded[i]);
            continue;
        }
        BUFFER_PUSH(embedded_types, embed_def->type);

        // Add promoted fields from embedded type
        for (int32_t j = 0; j < embed_def->type->struct_type.field_count; j++) {
            const StructField *ef = &embed_def->type->struct_type.fields[j];
            StructFieldInfo fi = {.name = ef->name, .type = ef->type, .default_value = NULL};
            // Find the default value from the embedded struct definition
            for (int32_t k = 0; k < BUFFER_LENGTH(embed_def->fields); k++) {
                if (strcmp(embed_def->fields[k].name, ef->name) == 0) {
                    fi.default_value = embed_def->fields[k].default_value;
                    break;
                }
            }
            BUFFER_PUSH(def->fields, fi);
        }
    }

    // Check for duplicates and add own fields
    for (int32_t i = 0; i < BUFFER_LENGTH(declaration->struct_declaration.fields); i++) {
        ASTStructField *ast_field = &declaration->struct_declaration.fields[i];
        bool duplicate = false;
        for (int32_t j = 0; j < BUFFER_LENGTH(def->fields); j++) {
            if (strcmp(def->fields[j].name, ast_field->name) == 0) {
                SEMA_ERROR(analyzer, declaration->location, "duplicate field '%s'",
                           ast_field->name);
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            const Type *field_type = resolve_ast_type(analyzer, &ast_field->type);
            if (field_type == NULL) {
                field_type = &TYPE_ERROR_INSTANCE;
            }
            StructFieldInfo fi = {.name = ast_field->name,
                                  .type = field_type,
                                  .default_value = ast_field->default_value};
            BUFFER_PUSH(def->fields, fi);
        }
    }

    // Build StructField array for the type
    StructField *type_fields = NULL;
    // Add embedded struct fields as a single named field (e.g., "Base")
    for (int32_t i = 0; i < BUFFER_LENGTH(def->embedded); i++) {
        StructDefinition *embed_def = find_struct_definition(analyzer, def->embedded[i]);
        if (embed_def != NULL) {
            StructField sf = {.name = def->embedded[i], .type = embed_def->type};
            BUFFER_PUSH(type_fields, sf);
        }
    }
    // Add own fields
    for (int32_t i = 0; i < BUFFER_LENGTH(declaration->struct_declaration.fields); i++) {
        ASTStructField *ast_field = &declaration->struct_declaration.fields[i];
        const Type *field_type = resolve_ast_type(analyzer, &ast_field->type);
        if (field_type == NULL) {
            field_type = &TYPE_ERROR_INSTANCE;
        }
        StructField sf = {.name = ast_field->name, .type = field_type};
        BUFFER_PUSH(type_fields, sf);
    }

    // Create the struct type
    def->type =
        type_create_struct(analyzer->arena, struct_name, type_fields, BUFFER_LENGTH(type_fields),
                           embedded_types, BUFFER_LENGTH(embedded_types));
    declaration->type = def->type;

    // Register methods
    for (int32_t i = 0; i < BUFFER_LENGTH(declaration->struct_declaration.methods); i++) {
        ASTNode *method = declaration->struct_declaration.methods[i];
        StructMethodInfo mi = {.name = method->function_declaration.name,
                               .is_mut_receiver = method->function_declaration.is_mut_receiver,
                               .receiver_name = method->function_declaration.receiver_name,
                               .declaration = method};
        BUFFER_PUSH(def->methods, mi);

        // Register method as function with key "StructName.method_name"
        const char *method_key = arena_sprintf(analyzer->arena, "%s.%s", struct_name, mi.name);

        const Type *return_type = &TYPE_UNIT_INSTANCE;
        if (method->function_declaration.return_type.kind != AST_TYPE_INFERRED) {
            return_type = resolve_ast_type(analyzer, &method->function_declaration.return_type);
            if (return_type == NULL) {
                return_type = &TYPE_UNIT_INSTANCE;
            }
        }

        FunctionSignature *sig = rsg_malloc(sizeof(*sig));
        sig->name = mi.name;
        sig->return_type = return_type;
        sig->parameter_types = NULL;
        sig->parameter_names = NULL;
        sig->parameter_count = BUFFER_LENGTH(method->function_declaration.parameters);
        sig->is_public = false;

        for (int32_t j = 0; j < sig->parameter_count; j++) {
            ASTNode *param = method->function_declaration.parameters[j];
            const Type *pt = resolve_ast_type(analyzer, &param->parameter.type);
            if (pt == NULL) {
                pt = &TYPE_ERROR_INSTANCE;
            }
            BUFFER_PUSH(sig->parameter_types, pt);
            BUFFER_PUSH(sig->parameter_names, param->parameter.name);
        }

        hash_table_insert(&analyzer->function_table, method_key, sig);
    }

    hash_table_insert(&analyzer->struct_table, struct_name, def);

    // Register struct as a type alias so resolve_ast_type can find it
    hash_table_insert(&analyzer->type_alias_table, struct_name, (void *)def->type);

    // Register struct name as a type symbol
    scope_define(analyzer, struct_name, def->type, false, SYM_TYPE);
}

bool semantic_analyzer_check(SemanticAnalyzer *analyzer, ASTNode *file) {
    // Reset tables for each compilation
    hash_table_destroy(&analyzer->function_table);
    hash_table_init(&analyzer->function_table, NULL);
    hash_table_destroy(&analyzer->type_alias_table);
    hash_table_init(&analyzer->type_alias_table, NULL);
    hash_table_destroy(&analyzer->struct_table);
    hash_table_init(&analyzer->struct_table, NULL);

    scope_push(analyzer, false); // global scope

    // First pass: register struct definitions, type aliases, and function signatures
    // Register structs first (they may be referenced by type aliases and functions)
    for (int32_t i = 0; i < BUFFER_LENGTH(file->file.declarations); i++) {
        ASTNode *declaration = file->file.declarations[i];
        if (declaration->kind == NODE_STRUCT_DECLARATION) {
            register_struct_definition(analyzer, declaration);
        }
    }

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
            register_function_signature(analyzer, declaration);
        }
    }

    // Second pass: type-check everything
    check_node(analyzer, file);

    scope_pop(analyzer);
    return analyzer->error_count == 0;
}
