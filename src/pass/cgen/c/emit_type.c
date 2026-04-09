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
            // Count non-unit fields to avoid empty or void-typed struct members
            int32_t real_count = 0;
            for (int32_t k = 0; k < v->tuple_count; k++) {
                if (v->tuple_types[k]->kind != TYPE_UNIT) {
                    real_count++;
                }
            }
            if (real_count > 0) {
                fprintf(cgen->output, " struct {");
                for (int32_t k = 0; k < v->tuple_count; k++) {
                    if (v->tuple_types[k]->kind == TYPE_UNIT) {
                        continue;
                    }
                    const char *ft = c_type_for(cgen, v->tuple_types[k]);
                    fprintf(cgen->output, " %s _%d;", ft, k);
                }
                fprintf(cgen->output, " } %s;", v->name);
            }
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

/** Check if a value-type dependency is emitted (or not compound). */
static bool dep_emitted(const Type **compounds, const bool *done, int32_t count, const Type *dep) {
    if (dep == NULL) {
        return true;
    }
    if (dep->kind == TYPE_PTR || dep->kind == TYPE_SLICE) {
        return true; // pointers/slices only need forward decl
    }
    if (dep->kind != TYPE_STRUCT && dep->kind != TYPE_ENUM && dep->kind != TYPE_ARRAY &&
        dep->kind != TYPE_TUPLE) {
        return true; // primitives always emitted
    }
    for (int32_t i = 0; i < count; i++) {
        if (compounds[i] == dep) {
            return done[i];
        }
    }
    return true; // not in compound_types, assume available
}

/** Check if all by-value deps of a compound type are emitted. */
static bool compound_deps_emitted(const Type **compounds, const bool *done, int32_t count,
                                  const Type *type) {
    switch (type->kind) {
    case TYPE_STRUCT:
        for (int32_t f = 0; f < type->struct_type.field_count; f++) {
            if (!dep_emitted(compounds, done, count, type->struct_type.fields[f].type)) {
                return false;
            }
        }
        return true;
    case TYPE_ENUM:
        for (int32_t v = 0; v < type->enum_type.variant_count; v++) {
            const EnumVariant *var = &type->enum_type.variants[v];
            if (var->kind == ENUM_VARIANT_TUPLE) {
                for (int32_t k = 0; k < var->tuple_count; k++) {
                    if (!dep_emitted(compounds, done, count, var->tuple_types[k])) {
                        return false;
                    }
                }
            } else if (var->kind == ENUM_VARIANT_STRUCT) {
                for (int32_t k = 0; k < var->field_count; k++) {
                    if (!dep_emitted(compounds, done, count, var->fields[k].type)) {
                        return false;
                    }
                }
            }
        }
        return true;
    case TYPE_ARRAY:
        return dep_emitted(compounds, done, count, type->array.elem);
    case TYPE_TUPLE:
        for (int32_t i = 0; i < type->tuple.count; i++) {
            if (!dep_emitted(compounds, done, count, type->tuple.elems[i])) {
                return false;
            }
        }
        return true;
    default:
        return true;
    }
}

// ── Compound type emission ────────────────────────────────────────────

/** Emitter function for a single compound type. */
typedef void (*CompoundTypeEmitter)(CGen *, const Type *, const char *);

/** Dispatch table indexed by TypeKind.  NULL entries are silently skipped. */
static const CompoundTypeEmitter COMPOUND_EMITTERS[] = {
    [TYPE_ARRAY] = emit_array_typedef,
    [TYPE_TUPLE] = emit_tuple_typedef,
    [TYPE_STRUCT] = emit_struct_typedef,
    [TYPE_ENUM] = emit_enum_typedef,
};

static const int32_t COMPOUND_EMITTERS_COUNT =
    (int32_t)(sizeof(COMPOUND_EMITTERS) / sizeof(COMPOUND_EMITTERS[0]));

void emit_compound_typedefs(CGen *cgen) {
    int32_t count = BUF_LEN(cgen->compound_types);
    if (count == 0) {
        return;
    }

    // Pass 1: emit forward typedef declarations for all compound types
    for (int32_t i = 0; i < count; i++) {
        const Type *type = cgen->compound_types[i];
        if (type_already_emitted(cgen->compound_types, i, type)) {
            continue;
        }
        const char *name = c_type_for(cgen, type);
        emit_line(cgen, "typedef struct %s %s;", name, name);
    }

    // Pass 2: emit full struct definitions in dependency order
    bool *done = calloc(count, sizeof(bool));
    for (int32_t i = 0; i < count; i++) {
        if (type_already_emitted(cgen->compound_types, i, cgen->compound_types[i])) {
            done[i] = true;
        }
    }

    int32_t remaining = 0;
    for (int32_t i = 0; i < count; i++) {
        if (!done[i]) {
            remaining++;
        }
    }

    while (remaining > 0) {
        bool progress = false;
        for (int32_t i = 0; i < count; i++) {
            if (done[i]) {
                continue;
            }
            const Type *type = cgen->compound_types[i];
            if (!compound_deps_emitted(cgen->compound_types, done, count, type)) {
                continue;
            }
            const char *name = c_type_for(cgen, type);
            if (type->kind < COMPOUND_EMITTERS_COUNT && COMPOUND_EMITTERS[type->kind] != NULL) {
                COMPOUND_EMITTERS[type->kind](cgen, type, name);
            }
            done[i] = true;
            remaining--;
            progress = true;
        }
        if (!progress) {
            // Circular or unresolvable — emit remaining in order
            for (int32_t i = 0; i < count; i++) {
                if (done[i]) {
                    continue;
                }
                const Type *type = cgen->compound_types[i];
                const char *name = c_type_for(cgen, type);
                if (type->kind < COMPOUND_EMITTERS_COUNT && COMPOUND_EMITTERS[type->kind] != NULL) {
                    COMPOUND_EMITTERS[type->kind](cgen, type, name);
                }
                done[i] = true;
            }
            remaining = 0;
        }
    }
    free(done);

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
