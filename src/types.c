#include "types.h"

// ---------------------------------------------------------------------------
// Type singletons — avoids allocation for primitive types.
// ---------------------------------------------------------------------------
Type g_type_bool_inst = {.kind = TYPE_BOOL};
Type g_type_i32_inst = {.kind = TYPE_I32};
Type g_type_u32_inst = {.kind = TYPE_U32};
Type g_type_f64_inst = {.kind = TYPE_F64};
Type g_type_str_inst = {.kind = TYPE_STR};
Type g_type_unit_inst = {.kind = TYPE_UNIT};
Type g_type_error_inst = {.kind = TYPE_ERROR};

// ---------------------------------------------------------------------------
// Shared type metadata table
// ---------------------------------------------------------------------------
static const struct {
    TypeKind kind;
    const char *rg_name;
    const char *c_name;
    Type *inst;
} TYPE_INFO[] = {
    {.kind = TYPE_BOOL, .rg_name = "bool", .c_name = "bool", .inst = &g_type_bool_inst},
    {.kind = TYPE_I32, .rg_name = "i32", .c_name = "int32_t", .inst = &g_type_i32_inst},
    {.kind = TYPE_U32, .rg_name = "u32", .c_name = "uint32_t", .inst = &g_type_u32_inst},
    {.kind = TYPE_F64, .rg_name = "f64", .c_name = "double", .inst = &g_type_f64_inst},
    {.kind = TYPE_STR, .rg_name = "str", .c_name = "RgStr", .inst = &g_type_str_inst},
    {.kind = TYPE_UNIT, .rg_name = "unit", .c_name = "void", .inst = &g_type_unit_inst},
    {.kind = TYPE_ERROR, .rg_name = "<error>", .c_name = "/* error */", .inst = &g_type_error_inst},
};

static const int32_t TYPE_INFO_COUNT = (int32_t)(sizeof(TYPE_INFO) / sizeof(TYPE_INFO[0]));

// ---------------------------------------------------------------------------
// Type utility functions
// ---------------------------------------------------------------------------
Type *type_from_name(const char *name) {
    if (name == NULL) {
        return NULL;
    }
    for (int32_t i = 0; i < TYPE_INFO_COUNT; i++) {
        if (TYPE_INFO[i].kind != TYPE_ERROR && strcmp(name, TYPE_INFO[i].rg_name) == 0) {
            return TYPE_INFO[i].inst;
        }
    }
    return NULL;
}

const char *type_name(const Type *t) {
    if (t == NULL) {
        return "<unknown>";
    }
    for (int32_t i = 0; i < TYPE_INFO_COUNT; i++) {
        if (TYPE_INFO[i].kind == t->kind) {
            return TYPE_INFO[i].rg_name;
        }
    }
    return "<unknown>";
}

const char *c_type_str(const Type *t) {
    if (t == NULL) {
        return "/* ? */";
    }
    for (int32_t i = 0; i < TYPE_INFO_COUNT; i++) {
        if (TYPE_INFO[i].kind == t->kind) {
            return TYPE_INFO[i].c_name;
        }
    }
    return "/* ? */";
}

bool type_eq(const Type *a, const Type *b) {
    if (a == NULL || b == NULL) {
        return false;
    }
    return a->kind == b->kind;
}

bool type_is_numeric(const Type *t) {
    return t != NULL && (t->kind == TYPE_I32 || t->kind == TYPE_U32 || t->kind == TYPE_F64);
}

bool type_is_integer(const Type *t) {
    return t != NULL && (t->kind == TYPE_I32 || t->kind == TYPE_U32);
}
