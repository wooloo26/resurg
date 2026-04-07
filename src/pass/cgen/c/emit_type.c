#include "helpers.h"

// ── Per-type typedef emitters ─────────────────────────────────────────

static void emit_array_typedef(CGen *cgen, const Type *type, const char *name) {
    emit_line(cgen, "struct %s { %s _data[%d]; };", name, c_type_for(cgen, type->array.elem),
              type->array.size);
}

static void emit_tuple_typedef(CGen *cgen, const Type *type, const char *name) {
    emit_indent(cgen);
    fprintf(cgen->output, "struct %s {", name);
    for (int32_t j = 0; j < type->tuple.count; j++) {
        const char *elem_type = c_type_for(cgen, type->tuple.elems[j]);
        fprintf(cgen->output, " %s _%d;", elem_type, j);
    }
    fprintf(cgen->output, " };\n");
}

static void emit_struct_typedef(CGen *cgen, const Type *type, const char *name) {
    emit_indent(cgen);
    fprintf(cgen->output, "struct %s {", name);
    for (int32_t j = 0; j < type->struct_type.field_count; j++) {
        const char *field_type = c_type_for(cgen, type->struct_type.fields[j].type);
        fprintf(cgen->output, " %s %s;", field_type, type->struct_type.fields[j].name);
    }
    fprintf(cgen->output, " };\n");
}

static void emit_enum_typedef(CGen *cgen, const Type *type, const char *name) {
    emit_indent(cgen);
    fprintf(cgen->output, "struct %s { int32_t _tag; union {", name);
    for (int32_t j = 0; j < type->enum_type.variant_count; j++) {
        const EnumVariant *v = &type->enum_type.variants[j];
        if (v->kind == ENUM_VARIANT_TUPLE) {
            fprintf(cgen->output, " struct {");
            for (int32_t k = 0; k < v->tuple_count; k++) {
                const char *ft = c_type_for(cgen, v->tuple_types[k]);
                fprintf(cgen->output, " %s _%d;", ft, k);
            }
            fprintf(cgen->output, " } %s;", v->name);
        } else if (v->kind == ENUM_VARIANT_STRUCT) {
            fprintf(cgen->output, " struct {");
            for (int32_t k = 0; k < v->field_count; k++) {
                const char *ft = c_type_for(cgen, v->fields[k].type);
                fprintf(cgen->output, " %s %s;", ft, v->fields[k].name);
            }
            fprintf(cgen->output, " } %s;", v->name);
        }
    }
    fprintf(cgen->output, " } _data; };\n");
}

/** Check if @p type already appears in @p types before index @p limit. */
static bool type_already_emitted(const Type **types, int32_t limit, const Type *type) {
    for (int32_t i = 0; i < limit; i++) {
        if (types[i] == type) {
            return true;
        }
    }
    return false;
}

// ── Compound type emission ────────────────────────────────────────────

// ── Compound type emission ────────────────────────────────────────────

void emit_compound_typedefs(CGen *cgen) {
    int32_t count = BUF_LEN(cgen->compound_types);

    // Pass 1: emit forward typedef declarations for all compound types
    for (int32_t i = 0; i < count; i++) {
        const Type *type = cgen->compound_types[i];
        if (type_already_emitted(cgen->compound_types, i, type)) {
            continue;
        }
        const char *name = c_type_for(cgen, type);
        emit_line(cgen, "typedef struct %s %s;", name, name);
    }

    // Pass 2: emit full struct definitions
    for (int32_t i = 0; i < count; i++) {
        const Type *type = cgen->compound_types[i];
        if (type_already_emitted(cgen->compound_types, i, type)) {
            continue;
        }
        const char *name = c_type_for(cgen, type);
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

    if (count > 0) {
        emit(cgen, "\n");
    }
}

void clear_compound_types(CGen *cgen) {
    if (cgen->compound_types != NULL) {
        BUF_FREE(cgen->compound_types);
        cgen->compound_types = NULL;
    }
}
