#include "_codegen.h"

// ── Compound type emission ────────────────────────────────────────────

void codegen_emit_compound_typedefs(CodeGenerator *generator) {
    for (int32_t i = 0; i < BUFFER_LENGTH(generator->compound_types); i++) {
        const Type *type = generator->compound_types[i];
        const char *name = codegen_c_type_for(generator, type);
        if (type->kind == TYPE_ARRAY) {
            codegen_emit_line(generator, "typedef struct { %s _data[%d]; } %s;",
                              codegen_c_type_for(generator, type->array.element), type->array.size,
                              name);
        } else if (type->kind == TYPE_TUPLE) {
            codegen_emit_indent(generator);
            fprintf(generator->output, "typedef struct {");
            for (int32_t j = 0; j < type->tuple.count; j++) {
                const char *elem_type = codegen_c_type_for(generator, type->tuple.elements[j]);
                fprintf(generator->output, " %s _%d;", elem_type, j);
            }
            fprintf(generator->output, " } %s;\n", name);
        } else if (type->kind == TYPE_STRUCT) {
            codegen_emit_indent(generator);
            fprintf(generator->output, "typedef struct {");
            for (int32_t j = 0; j < type->struct_type.field_count; j++) {
                const char *field_type =
                    codegen_c_type_for(generator, type->struct_type.fields[j].type);
                fprintf(generator->output, " %s %s;", field_type, type->struct_type.fields[j].name);
            }
            fprintf(generator->output, " } %s;\n", name);
        }
    }
    if (BUFFER_LENGTH(generator->compound_types) > 0) {
        codegen_emit(generator, "\n");
    }
}

void codegen_reset_compound_types(CodeGenerator *generator) {
    if (generator->compound_types != NULL) {
        BUFFER_FREE(generator->compound_types);
        generator->compound_types = NULL;
    }
}
