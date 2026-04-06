#include "_cgen.h"

// ── Per-type typedef emitters ─────────────────────────────────────────

static void emit_array_typedef(CGen *cgen, const Type *type, const char *name) {
    cgen_emit_line(cgen, "typedef struct { %s _data[%d]; } %s;",
                   cgen_c_type_for(cgen, type->array.elem), type->array.size, name);
}

static void emit_tuple_typedef(CGen *cgen, const Type *type, const char *name) {
    cgen_emit_indent(cgen);
    fprintf(cgen->output, "typedef struct {");
    for (int32_t j = 0; j < type->tuple.count; j++) {
        const char *elem_type = cgen_c_type_for(cgen, type->tuple.elems[j]);
        fprintf(cgen->output, " %s _%d;", elem_type, j);
    }
    fprintf(cgen->output, " } %s;\n", name);
}

static void emit_struct_typedef(CGen *cgen, const Type *type, const char *name) {
    cgen_emit_indent(cgen);
    fprintf(cgen->output, "typedef struct {");
    for (int32_t j = 0; j < type->struct_type.field_count; j++) {
        const char *field_type = cgen_c_type_for(cgen, type->struct_type.fields[j].type);
        fprintf(cgen->output, " %s %s;", field_type, type->struct_type.fields[j].name);
    }
    fprintf(cgen->output, " } %s;\n", name);
}

static void emit_enum_typedef(CGen *cgen, const Type *type, const char *name) {
    cgen_emit_indent(cgen);
    fprintf(cgen->output, "typedef struct { int32_t _tag; union {");
    for (int32_t j = 0; j < type->enum_type.variant_count; j++) {
        const EnumVariant *v = &type->enum_type.variants[j];
        if (v->kind == ENUM_VARIANT_TUPLE) {
            fprintf(cgen->output, " struct {");
            for (int32_t k = 0; k < v->tuple_count; k++) {
                const char *ft = cgen_c_type_for(cgen, v->tuple_types[k]);
                fprintf(cgen->output, " %s _%d;", ft, k);
            }
            fprintf(cgen->output, " } %s;", v->name);
        } else if (v->kind == ENUM_VARIANT_STRUCT) {
            fprintf(cgen->output, " struct {");
            for (int32_t k = 0; k < v->field_count; k++) {
                const char *ft = cgen_c_type_for(cgen, v->fields[k].type);
                fprintf(cgen->output, " %s %s;", ft, v->fields[k].name);
            }
            fprintf(cgen->output, " } %s;", v->name);
        }
    }
    fprintf(cgen->output, " } _data; } %s;\n", name);
}

// ── Compound type emission ────────────────────────────────────────────

void cgen_emit_compound_typedefs(CGen *cgen) {
    for (int32_t i = 0; i < BUF_LEN(cgen->compound_types); i++) {
        const Type *type = cgen->compound_types[i];

        // Skip duplicate types already emitted
        bool duplicate = false;
        for (int32_t d = 0; d < i; d++) {
            if (cgen->compound_types[d] == type) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }

        const char *name = cgen_c_type_for(cgen, type);
        if (type->kind == TYPE_ARRAY) {
            emit_array_typedef(cgen, type, name);
        } else if (type->kind == TYPE_TUPLE) {
            emit_tuple_typedef(cgen, type, name);
        } else if (type->kind == TYPE_STRUCT) {
            emit_struct_typedef(cgen, type, name);
        } else if (type->kind == TYPE_ENUM) {
            emit_enum_typedef(cgen, type, name);
        }
    }
    if (BUF_LEN(cgen->compound_types) > 0) {
        _cgen_emit(cgen, "\n");
    }
}

void cgen_clear_compound_types(CGen *cgen) {
    if (cgen->compound_types != NULL) {
        BUF_FREE(cgen->compound_types);
        cgen->compound_types = NULL;
    }
}
