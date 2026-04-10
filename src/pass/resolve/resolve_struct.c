#include "_sema.h"

/**
 * @file resolve_struct.c
 * @brief Struct definition registration — fields, embedded types, methods.
 */

// ── Static helpers ─────────────────────────────────────────────────

/**
 * Collect fields for a struct: promoted fields from embedded structs first,
 * then the struct's own fields (with duplicate checking).
 */
static void compose_struct_fields(Sema *sema, const ASTNode *decl, StructDef *def) {
    // Promote fields from embedded structs
    for (int32_t i = 0; i < BUF_LEN(def->embedded); i++) {
        StructDef *embed_def = sema_lookup_struct(sema, def->embedded[i]);
        if (embed_def == NULL) {
            SEMA_ERR(sema, decl->loc, "unknown embedded struct '%s'", def->embedded[i]);
            continue;
        }
        for (int32_t j = 0; j < embed_def->type->struct_type.field_count; j++) {
            const StructField *ef = &embed_def->type->struct_type.fields[j];
            StructFieldInfo fi = {
                .name = ef->name, .type = ef->type, .default_value = NULL, .is_pub = ef->is_pub};
            for (int32_t k = 0; k < BUF_LEN(embed_def->fields); k++) {
                if (strcmp(embed_def->fields[k].name, ef->name) == 0) {
                    fi.default_value = embed_def->fields[k].default_value;
                    break;
                }
            }
            BUF_PUSH(def->fields, fi);
        }
    }

    // Add own fields, checking for duplicates against promoted fields
    for (int32_t i = 0; i < BUF_LEN(decl->struct_decl.fields); i++) {
        ASTStructField *ast_field = &decl->struct_decl.fields[i];
        bool duplicate = false;
        for (int32_t j = 0; j < BUF_LEN(def->fields); j++) {
            if (strcmp(def->fields[j].name, ast_field->name) == 0) {
                SEMA_ERR(sema, decl->loc, "duplicate field '%s'", ast_field->name);
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            const Type *field_type = resolve_ast_type(sema, &ast_field->type);
            if (field_type == NULL) {
                field_type = &TYPE_ERR_INST;
            }
            StructFieldInfo fi = {.name = ast_field->name,
                                  .type = field_type,
                                  .default_value = ast_field->default_value,
                                  .is_pub = ast_field->is_pub};
            BUF_PUSH(def->fields, fi);
        }
    }
}

/**
 * Build the Type* for a struct from its collected fields and embedded types,
 * and assign it to both the def and the AST decl node.
 */
static void build_struct_type(Sema *sema, ASTNode *decl, StructDef *def) {
    const Type **embedded_types = NULL;
    StructField *type_fields = NULL;

    // Add embedded struct fields as named fields (e.g., "Base")
    for (int32_t i = 0; i < BUF_LEN(def->embedded); i++) {
        StructDef *embed_def = sema_lookup_struct(sema, def->embedded[i]);
        if (embed_def != NULL) {
            BUF_PUSH(embedded_types, embed_def->type);
            StructField sf = {.name = def->embedded[i], .type = embed_def->type, .is_pub = true};
            BUF_PUSH(type_fields, sf);
        }
    }

    // Add own fields
    for (int32_t i = 0; i < BUF_LEN(decl->struct_decl.fields); i++) {
        ASTStructField *ast_field = &decl->struct_decl.fields[i];
        const Type *field_type = resolve_ast_type(sema, &ast_field->type);
        if (field_type == NULL) {
            field_type = &TYPE_ERR_INST;
        }
        StructField sf = {.name = ast_field->name, .type = field_type, .is_pub = ast_field->is_pub};
        BUF_PUSH(type_fields, sf);
    }

    StructTypeSpec struct_spec = {
        .name = def->name,
        .fields = type_fields,
        .field_count = BUF_LEN(type_fields),
        .embedded = embedded_types,
        .embed_count = BUF_LEN(embedded_types),
    };
    def->type = type_create_struct(sema->arena, &struct_spec);
    decl->type = def->type;
}

/** Register each struct method as a fn sig. */
static void register_struct_methods(Sema *sema, const ASTNode *decl, StructDef *def) {
    for (int32_t i = 0; i < BUF_LEN(decl->struct_decl.methods); i++) {
        register_method_sig(sema, def->name, decl->struct_decl.methods[i], &def->methods);
    }
}

// ── Public API ─────────────────────────────────────────────────────

void register_struct_def(Sema *sema, ASTNode *decl) {
    const char *struct_name = decl->struct_decl.name;

    // If the struct has type params, store as a generic template instead
    if (BUF_LEN(decl->struct_decl.type_params) > 0) {
        GenericStructDef *gdef = rsg_malloc(sizeof(*gdef));
        gdef->name = struct_name;
        gdef->decl = decl;
        gdef->type_params = decl->struct_decl.type_params;
        gdef->type_param_count = BUF_LEN(decl->struct_decl.type_params);
        hash_table_insert(&sema->generics.structs, struct_name, gdef);
        return;
    }

    // Check for duplicate struct def
    if (sema_lookup_struct(sema, struct_name) != NULL) {
        SEMA_ERR(sema, decl->loc, "duplicate struct def '%s'", struct_name);
        return;
    }

    const char *prev_self = sema->infer.self_type_name;
    sema->infer.self_type_name = struct_name;

    StructDef *def = rsg_malloc(sizeof(*def));
    def->name = struct_name;
    def->is_tuple_struct = decl->struct_decl.is_tuple_struct;
    def->fields = NULL;
    def->methods = NULL;
    def->embedded = NULL;
    def->assoc_types = NULL;

    // Copy associated types from AST
    for (int32_t i = 0; i < BUF_LEN(decl->struct_decl.assoc_types); i++) {
        BUF_PUSH(def->assoc_types, decl->struct_decl.assoc_types[i]);
    }

    // Collect embedded struct types
    for (int32_t i = 0; i < BUF_LEN(decl->struct_decl.embedded); i++) {
        BUF_PUSH(def->embedded, decl->struct_decl.embedded[i]);
    }

    compose_struct_fields(sema, decl, def);
    build_struct_type(sema, decl, def);

    // Register type alias early so Self-referencing method sigs can resolve
    hash_table_insert(&sema->db.struct_table, struct_name, def);
    hash_table_insert(&sema->db.type_alias_table, struct_name, (void *)def->type);
    scope_define(sema, &(SymDef){struct_name, def->type, false, SYM_TYPE});

    register_struct_methods(sema, decl, def);

    sema->infer.self_type_name = prev_self;
}
