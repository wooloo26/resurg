#include "_codegen.h"

// ── Per-type typedef emitters ─────────────────────────────────────────

static void emit_array_typedef(CodeGenerator *generator, const Type *type, const char *name) {
    codegen_emit_line(generator, "typedef struct { %s _data[%d]; } %s;",
                      codegen_c_type_for(generator, type->array.element), type->array.size, name);
}

static void emit_tuple_typedef(CodeGenerator *generator, const Type *type, const char *name) {
    codegen_emit_indent(generator);
    fprintf(generator->output, "typedef struct {");
    for (int32_t j = 0; j < type->tuple.count; j++) {
        const char *elem_type = codegen_c_type_for(generator, type->tuple.elements[j]);
        fprintf(generator->output, " %s _%d;", elem_type, j);
    }
    fprintf(generator->output, " } %s;\n", name);
}

static void emit_struct_typedef(CodeGenerator *generator, const Type *type, const char *name) {
    codegen_emit_indent(generator);
    fprintf(generator->output, "typedef struct {");
    for (int32_t j = 0; j < type->struct_type.field_count; j++) {
        const char *field_type = codegen_c_type_for(generator, type->struct_type.fields[j].type);
        fprintf(generator->output, " %s %s;", field_type, type->struct_type.fields[j].name);
    }
    fprintf(generator->output, " } %s;\n", name);
}

static void emit_enum_typedef(CodeGenerator *generator, const Type *type, const char *name) {
    codegen_emit_indent(generator);
    fprintf(generator->output, "typedef struct { int32_t _tag; union {");
    for (int32_t j = 0; j < type->enum_type.variant_count; j++) {
        const EnumVariant *v = &type->enum_type.variants[j];
        if (v->kind == ENUM_VARIANT_TUPLE) {
            fprintf(generator->output, " struct {");
            for (int32_t k = 0; k < v->tuple_count; k++) {
                const char *ft = codegen_c_type_for(generator, v->tuple_types[k]);
                fprintf(generator->output, " %s _%d;", ft, k);
            }
            fprintf(generator->output, " } %s;", v->name);
        } else if (v->kind == ENUM_VARIANT_STRUCT) {
            fprintf(generator->output, " struct {");
            for (int32_t k = 0; k < v->field_count; k++) {
                const char *ft = codegen_c_type_for(generator, v->fields[k].type);
                fprintf(generator->output, " %s %s;", ft, v->fields[k].name);
            }
            fprintf(generator->output, " } %s;", v->name);
        }
    }
    fprintf(generator->output, " } _data; } %s;\n", name);
}

// ── Compound type emission ────────────────────────────────────────────

void codegen_emit_compound_typedefs(CodeGenerator *generator) {
    for (int32_t i = 0; i < BUFFER_LENGTH(generator->compound_types); i++) {
        const Type *type = generator->compound_types[i];

        // Skip duplicate types already emitted
        bool duplicate = false;
        for (int32_t d = 0; d < i; d++) {
            if (generator->compound_types[d] == type) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }

        const char *name = codegen_c_type_for(generator, type);
        if (type->kind == TYPE_ARRAY) {
            emit_array_typedef(generator, type, name);
        } else if (type->kind == TYPE_TUPLE) {
            emit_tuple_typedef(generator, type, name);
        } else if (type->kind == TYPE_STRUCT) {
            emit_struct_typedef(generator, type, name);
        } else if (type->kind == TYPE_ENUM) {
            emit_enum_typedef(generator, type, name);
        }
    }
    if (BUFFER_LENGTH(generator->compound_types) > 0) {
        codegen_emit(generator, "\n");
    }
}

void codegen_clear_compound_types(CodeGenerator *generator) {
    if (generator->compound_types != NULL) {
        BUFFER_FREE(generator->compound_types);
        generator->compound_types = NULL;
    }
}
