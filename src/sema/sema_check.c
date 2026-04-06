#include "_sema.h"

// ── Public API ─────────────────────────────────────────────────────────

SemanticAnalyzer *semantic_analyzer_create(Arena *arena) {
    SemanticAnalyzer *analyzer = rsg_malloc(sizeof(*analyzer));
    analyzer->arena = arena;
    analyzer->current_scope = NULL;
    analyzer->error_count = 0;
    analyzer->loop_break_type = NULL;
    hash_table_init(&analyzer->type_alias_table, NULL);
    hash_table_init(&analyzer->function_table, NULL);
    hash_table_init(&analyzer->struct_table, NULL);
    hash_table_init(&analyzer->enum_table, NULL);
    hash_table_init(&analyzer->pact_table, NULL);
    return analyzer;
}

void semantic_analyzer_destroy(SemanticAnalyzer *analyzer) {
    if (analyzer != NULL) {
        hash_table_destroy(&analyzer->type_alias_table);
        hash_table_destroy(&analyzer->function_table);
        hash_table_destroy(&analyzer->struct_table);
        hash_table_destroy(&analyzer->enum_table);
        hash_table_destroy(&analyzer->pact_table);
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

    scope_define(analyzer, &(SymbolDef){declaration->function_declaration.name, resolved_return,
                                        declaration->function_declaration.is_public, SYM_FUNCTION});
}

/**
 * Collect fields for a struct: promoted fields from embedded structs first,
 * then the struct's own fields (with duplicate checking).
 */
static void compose_struct_fields(SemanticAnalyzer *analyzer, const ASTNode *declaration,
                                  StructDefinition *def) {
    // Promote fields from embedded structs
    for (int32_t i = 0; i < BUFFER_LENGTH(def->embedded); i++) {
        StructDefinition *embed_def = sema_lookup_struct(analyzer, def->embedded[i]);
        if (embed_def == NULL) {
            SEMA_ERROR(analyzer, declaration->location, "unknown embedded struct '%s'",
                       def->embedded[i]);
            continue;
        }
        for (int32_t j = 0; j < embed_def->type->struct_type.field_count; j++) {
            const StructField *ef = &embed_def->type->struct_type.fields[j];
            StructFieldInfo fi = {.name = ef->name, .type = ef->type, .default_value = NULL};
            for (int32_t k = 0; k < BUFFER_LENGTH(embed_def->fields); k++) {
                if (strcmp(embed_def->fields[k].name, ef->name) == 0) {
                    fi.default_value = embed_def->fields[k].default_value;
                    break;
                }
            }
            BUFFER_PUSH(def->fields, fi);
        }
    }

    // Add own fields, checking for duplicates against promoted fields
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
}

/**
 * Build the Type* for a struct from its collected fields and embedded types,
 * and assign it to both the definition and the AST declaration node.
 */
static void build_struct_type(SemanticAnalyzer *analyzer, ASTNode *declaration,
                              StructDefinition *def) {
    const Type **embedded_types = NULL;
    StructField *type_fields = NULL;

    // Add embedded struct fields as named fields (e.g., "Base")
    for (int32_t i = 0; i < BUFFER_LENGTH(def->embedded); i++) {
        StructDefinition *embed_def = sema_lookup_struct(analyzer, def->embedded[i]);
        if (embed_def != NULL) {
            BUFFER_PUSH(embedded_types, embed_def->type);
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

    def->type =
        type_create_struct(analyzer->arena, def->name, type_fields, BUFFER_LENGTH(type_fields),
                           embedded_types, BUFFER_LENGTH(embedded_types));
    declaration->type = def->type;
}

/**
 * Register a method's signature in the function table (keyed as "TypeName.method_name")
 * and append a StructMethodInfo entry to the methods buffer.
 */
static void register_method_signature(SemanticAnalyzer *analyzer, const char *type_name,
                                      ASTNode *method, StructMethodInfo **methods) {
    StructMethodInfo mi = {.name = method->function_declaration.name,
                           .is_mut_receiver = method->function_declaration.is_mut_receiver,
                           .is_pointer_receiver = method->function_declaration.is_pointer_receiver,
                           .receiver_name = method->function_declaration.receiver_name,
                           .declaration = method};
    BUFFER_PUSH(*methods, mi);

    const char *method_key = arena_sprintf(analyzer->arena, "%s.%s", type_name, mi.name);

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

/**
 * Register each struct method as a function signature in the analyzer's
 * function table (keyed as "StructName.method_name").
 */
static void register_struct_methods(SemanticAnalyzer *analyzer, const ASTNode *declaration,
                                    StructDefinition *def) {
    for (int32_t i = 0; i < BUFFER_LENGTH(declaration->struct_declaration.methods); i++) {
        register_method_signature(analyzer, def->name, declaration->struct_declaration.methods[i],
                                  &def->methods);
    }
}

/** Register a struct definition: build the TYPE_STRUCT, register methods as functions. */
static void register_struct_definition(SemanticAnalyzer *analyzer, ASTNode *declaration) {
    const char *struct_name = declaration->struct_declaration.name;

    // Check for duplicate struct definition
    if (sema_lookup_struct(analyzer, struct_name) != NULL) {
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

    compose_struct_fields(analyzer, declaration, def);
    build_struct_type(analyzer, declaration, def);
    register_struct_methods(analyzer, declaration, def);

    hash_table_insert(&analyzer->struct_table, struct_name, def);

    // Register struct as a type alias so resolve_ast_type can find it
    hash_table_insert(&analyzer->type_alias_table, struct_name, (void *)def->type);

    // Register struct name as a type symbol
    scope_define(analyzer, &(SymbolDef){struct_name, def->type, false, SYM_TYPE});
}

/** Register an enum definition: build the TYPE_ENUM, register methods as functions. */
/** Build a single EnumVariant from its AST representation. */
static EnumVariant build_enum_variant(SemanticAnalyzer *analyzer, ASTEnumVariant *ast_variant,
                                      int32_t *auto_discriminant) {
    EnumVariant variant = {0};
    variant.name = ast_variant->name;

    switch (ast_variant->kind) {
    case VARIANT_UNIT:
        variant.kind = ENUM_VARIANT_UNIT;
        break;
    case VARIANT_TUPLE: {
        variant.kind = ENUM_VARIANT_TUPLE;
        variant.tuple_count = BUFFER_LENGTH(ast_variant->tuple_types);
        variant.tuple_types = (const Type **)arena_alloc_zero(
            analyzer->arena, variant.tuple_count * sizeof(const Type *));
        for (int32_t j = 0; j < variant.tuple_count; j++) {
            variant.tuple_types[j] = resolve_ast_type(analyzer, &ast_variant->tuple_types[j]);
            if (variant.tuple_types[j] == NULL) {
                variant.tuple_types[j] = &TYPE_ERROR_INSTANCE;
            }
        }
        break;
    }
    case VARIANT_STRUCT: {
        variant.kind = ENUM_VARIANT_STRUCT;
        variant.field_count = BUFFER_LENGTH(ast_variant->fields);
        variant.fields =
            arena_alloc_zero(analyzer->arena, variant.field_count * sizeof(StructField));
        for (int32_t j = 0; j < variant.field_count; j++) {
            variant.fields[j].name = ast_variant->fields[j].name;
            variant.fields[j].type = resolve_ast_type(analyzer, &ast_variant->fields[j].type);
            if (variant.fields[j].type == NULL) {
                variant.fields[j].type = &TYPE_ERROR_INSTANCE;
            }
        }
        break;
    }
    }

    if (ast_variant->discriminant != NULL) {
        if (ast_variant->discriminant->kind == NODE_LITERAL &&
            ast_variant->discriminant->literal.kind == LITERAL_I32) {
            variant.discriminant = (int32_t)ast_variant->discriminant->literal.integer_value;
            *auto_discriminant = (int32_t)variant.discriminant + 1;
        }
    } else {
        variant.discriminant = *auto_discriminant;
        (*auto_discriminant)++;
    }
    return variant;
}

static void register_enum_definition(SemanticAnalyzer *analyzer, ASTNode *declaration) {
    const char *enum_name = declaration->enum_declaration.name;

    if (sema_lookup_enum(analyzer, enum_name) != NULL) {
        SEMA_ERROR(analyzer, declaration->location, "duplicate enum definition '%s'", enum_name);
        return;
    }

    EnumVariant *variants = NULL;
    int32_t auto_discriminant = 0;
    for (int32_t i = 0; i < BUFFER_LENGTH(declaration->enum_declaration.variants); i++) {
        EnumVariant variant = build_enum_variant(
            analyzer, &declaration->enum_declaration.variants[i], &auto_discriminant);
        BUFFER_PUSH(variants, variant);
    }

    const Type *enum_type =
        type_create_enum(analyzer->arena, enum_name, variants, BUFFER_LENGTH(variants));
    declaration->type = enum_type;

    EnumDefinition *def = rsg_malloc(sizeof(*def));
    def->name = enum_name;
    def->methods = NULL;
    def->type = enum_type;

    for (int32_t i = 0; i < BUFFER_LENGTH(declaration->enum_declaration.methods); i++) {
        register_method_signature(analyzer, enum_name, declaration->enum_declaration.methods[i],
                                  &def->methods);
    }

    hash_table_insert(&analyzer->enum_table, enum_name, def);
    hash_table_insert(&analyzer->type_alias_table, enum_name, (void *)enum_type);
    scope_define(analyzer, &(SymbolDef){enum_name, enum_type, false, SYM_TYPE});
}

/**
 * Collect all required fields from a pact and its super pacts (recursively).
 * Appends to @p fields buffer.
 */
static void collect_pact_fields(SemanticAnalyzer *analyzer, const PactDefinition *pact,
                                StructFieldInfo **fields, SourceLocation location) {
    // Recurse into super pacts
    for (int32_t i = 0; i < BUFFER_LENGTH(pact->super_pacts); i++) {
        PactDefinition *super = sema_lookup_pact(analyzer, pact->super_pacts[i]);
        if (super != NULL) {
            collect_pact_fields(analyzer, super, fields, location);
        }
    }
    // Add this pact's own fields (avoid duplicates)
    for (int32_t i = 0; i < BUFFER_LENGTH(pact->fields); i++) {
        bool exists = false;
        for (int32_t j = 0; j < BUFFER_LENGTH(*fields); j++) {
            if (strcmp((*fields)[j].name, pact->fields[i].name) == 0) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            BUFFER_PUSH(*fields, pact->fields[i]);
        }
    }
}

/**
 * Collect all required methods from a pact and its super pacts (recursively).
 * Appends to @p methods buffer.
 */
static void collect_pact_methods(SemanticAnalyzer *analyzer, const PactDefinition *pact,
                                 StructMethodInfo **methods, SourceLocation location) {
    for (int32_t i = 0; i < BUFFER_LENGTH(pact->super_pacts); i++) {
        PactDefinition *super = sema_lookup_pact(analyzer, pact->super_pacts[i]);
        if (super != NULL) {
            collect_pact_methods(analyzer, super, methods, location);
        }
    }
    for (int32_t i = 0; i < BUFFER_LENGTH(pact->methods); i++) {
        bool exists = false;
        for (int32_t j = 0; j < BUFFER_LENGTH(*methods); j++) {
            if (strcmp((*methods)[j].name, pact->methods[i].name) == 0) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            BUFFER_PUSH(*methods, pact->methods[i]);
        }
    }
}

/** Register a pact definition during pass 1. */
static void register_pact_definition(SemanticAnalyzer *analyzer, ASTNode *declaration) {
    const char *pact_name = declaration->pact_declaration.name;

    if (sema_lookup_pact(analyzer, pact_name) != NULL) {
        SEMA_ERROR(analyzer, declaration->location, "duplicate pact definition '%s'", pact_name);
        return;
    }

    PactDefinition *def = rsg_malloc(sizeof(*def));
    def->name = pact_name;
    def->fields = NULL;
    def->methods = NULL;
    def->super_pacts = NULL;

    // Copy super pact references
    for (int32_t i = 0; i < BUFFER_LENGTH(declaration->pact_declaration.super_pacts); i++) {
        BUFFER_PUSH(def->super_pacts, declaration->pact_declaration.super_pacts[i]);
    }

    // Register required fields
    for (int32_t i = 0; i < BUFFER_LENGTH(declaration->pact_declaration.fields); i++) {
        ASTStructField *ast_field = &declaration->pact_declaration.fields[i];
        const Type *field_type = resolve_ast_type(analyzer, &ast_field->type);
        if (field_type == NULL) {
            field_type = &TYPE_ERROR_INSTANCE;
        }
        StructFieldInfo fi = {.name = ast_field->name, .type = field_type, .default_value = NULL};
        BUFFER_PUSH(def->fields, fi);
    }

    // Register methods
    for (int32_t i = 0; i < BUFFER_LENGTH(declaration->pact_declaration.methods); i++) {
        ASTNode *method = declaration->pact_declaration.methods[i];
        StructMethodInfo mi = {.name = method->function_declaration.name,
                               .is_mut_receiver = method->function_declaration.is_mut_receiver,
                               .is_pointer_receiver =
                                   method->function_declaration.is_pointer_receiver,
                               .receiver_name = method->function_declaration.receiver_name,
                               .declaration = method};
        BUFFER_PUSH(def->methods, mi);
    }

    hash_table_insert(&analyzer->pact_table, pact_name, def);
}

/**
 * Validate that a struct satisfies all pact conformances.
 * Checks required fields and methods exist, and injects default methods.
 */
static void validate_struct_conformances(SemanticAnalyzer *analyzer, ASTNode *declaration,
                                         StructDefinition *def) {
    for (int32_t ci = 0; ci < BUFFER_LENGTH(declaration->struct_declaration.conformances); ci++) {
        const char *pact_name = declaration->struct_declaration.conformances[ci];
        PactDefinition *pact = sema_lookup_pact(analyzer, pact_name);
        if (pact == NULL) {
            SEMA_ERROR(analyzer, declaration->location, "unknown pact '%s'", pact_name);
            continue;
        }

        // Collect all required fields from this pact and its super pacts
        StructFieldInfo *required_fields = NULL;
        collect_pact_fields(analyzer, pact, &required_fields, declaration->location);

        // Check required fields exist in struct
        for (int32_t i = 0; i < BUFFER_LENGTH(required_fields); i++) {
            bool found = false;
            for (int32_t j = 0; j < BUFFER_LENGTH(def->fields); j++) {
                if (strcmp(def->fields[j].name, required_fields[i].name) == 0) {
                    if (!type_equal(def->fields[j].type, required_fields[i].type)) {
                        SEMA_ERROR(analyzer, declaration->location,
                                   "field '%s' has type '%s' but pact '%s' requires type '%s'",
                                   required_fields[i].name,
                                   type_name(analyzer->arena, def->fields[j].type), pact_name,
                                   type_name(analyzer->arena, required_fields[i].type));
                    }
                    found = true;
                    break;
                }
            }
            if (!found) {
                SEMA_ERROR(analyzer, declaration->location,
                           "missing required field '%s' from pact '%s'", required_fields[i].name,
                           pact_name);
            }
        }
        BUFFER_FREE(required_fields);

        // Collect all required methods from this pact and its super pacts
        StructMethodInfo *pact_methods = NULL;
        collect_pact_methods(analyzer, pact, &pact_methods, declaration->location);

        // Check required methods exist or inject default implementations
        for (int32_t i = 0; i < BUFFER_LENGTH(pact_methods); i++) {
            bool has_body = pact_methods[i].declaration != NULL &&
                            pact_methods[i].declaration->function_declaration.body != NULL;

            // Check if struct already has this method
            bool found = false;
            for (int32_t j = 0; j < BUFFER_LENGTH(def->methods); j++) {
                if (strcmp(def->methods[j].name, pact_methods[i].name) == 0) {
                    found = true;
                    break;
                }
            }

            if (!found) {
                if (has_body) {
                    // Inject default method into struct
                    ASTNode *method_ast = pact_methods[i].declaration;
                    method_ast->function_declaration.owner_struct = def->name;
                    BUFFER_PUSH(declaration->struct_declaration.methods, method_ast);
                    register_method_signature(analyzer, def->name, method_ast, &def->methods);
                } else {
                    SEMA_ERROR(analyzer, declaration->location,
                               "missing required method '%s' from pact '%s'", pact_methods[i].name,
                               pact_name);
                }
            }
        }
        BUFFER_FREE(pact_methods);
    }
}

bool semantic_analyzer_check(SemanticAnalyzer *analyzer, ASTNode *file) {
    // Reset tables for each compilation
    hash_table_destroy(&analyzer->function_table);
    hash_table_init(&analyzer->function_table, NULL);
    hash_table_destroy(&analyzer->type_alias_table);
    hash_table_init(&analyzer->type_alias_table, NULL);
    hash_table_destroy(&analyzer->struct_table);
    hash_table_init(&analyzer->struct_table, NULL);
    hash_table_destroy(&analyzer->enum_table);
    hash_table_init(&analyzer->enum_table, NULL);
    hash_table_destroy(&analyzer->pact_table);
    hash_table_init(&analyzer->pact_table, NULL);

    scope_push(analyzer, false); // global scope

    // First pass: register struct definitions, type aliases, and function signatures

    // Register pacts first (they must be available when validating struct conformances)
    for (int32_t i = 0; i < BUFFER_LENGTH(file->file.declarations); i++) {
        ASTNode *declaration = file->file.declarations[i];
        if (declaration->kind == NODE_PACT_DECLARATION) {
            register_pact_definition(analyzer, declaration);
        }
    }

    // Register structs (they may be referenced by type aliases and functions)
    for (int32_t i = 0; i < BUFFER_LENGTH(file->file.declarations); i++) {
        ASTNode *declaration = file->file.declarations[i];
        if (declaration->kind == NODE_STRUCT_DECLARATION) {
            register_struct_definition(analyzer, declaration);
        }
    }

    // Validate pact conformances after all structs and pacts are registered
    for (int32_t i = 0; i < BUFFER_LENGTH(file->file.declarations); i++) {
        ASTNode *declaration = file->file.declarations[i];
        if (declaration->kind == NODE_STRUCT_DECLARATION &&
            BUFFER_LENGTH(declaration->struct_declaration.conformances) > 0) {
            StructDefinition *def =
                sema_lookup_struct(analyzer, declaration->struct_declaration.name);
            if (def != NULL) {
                validate_struct_conformances(analyzer, declaration, def);
            }
        }
    }

    // Register enum definitions
    for (int32_t i = 0; i < BUFFER_LENGTH(file->file.declarations); i++) {
        ASTNode *declaration = file->file.declarations[i];
        if (declaration->kind == NODE_ENUM_DECLARATION) {
            register_enum_definition(analyzer, declaration);
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
