#include "types.h"

// ---------------------------------------------------------------------------
// Type singletons — avoids allocation for primitive types.
// ---------------------------------------------------------------------------
const Type TYPE_BOOL_INST = {.kind = TYPE_BOOL};
const Type TYPE_I32_INST = {.kind = TYPE_I32};
const Type TYPE_U32_INST = {.kind = TYPE_U32};
const Type TYPE_F64_INST = {.kind = TYPE_F64};
const Type TYPE_STR_INST = {.kind = TYPE_STR};
const Type TYPE_UNIT_INST = {.kind = TYPE_UNIT};
const Type TYPE_ERROR_INST = {.kind = TYPE_ERROR};

// ---------------------------------------------------------------------------
// Shared type metadata table
// ---------------------------------------------------------------------------
static const struct {
    TypeKind kind;
    const char *rg_name;
    const char *c_name;
    const Type *inst;
} TYPE_INFO[] = {
    {.kind = TYPE_BOOL, .rg_name = "bool", .c_name = "bool", .inst = &TYPE_BOOL_INST},
    {.kind = TYPE_I32, .rg_name = "i32", .c_name = "int32_t", .inst = &TYPE_I32_INST},
    {.kind = TYPE_U32, .rg_name = "u32", .c_name = "uint32_t", .inst = &TYPE_U32_INST},
    {.kind = TYPE_F64, .rg_name = "f64", .c_name = "double", .inst = &TYPE_F64_INST},
    {.kind = TYPE_STR, .rg_name = "str", .c_name = "RgStr", .inst = &TYPE_STR_INST},
    {.kind = TYPE_UNIT, .rg_name = "unit", .c_name = "void", .inst = &TYPE_UNIT_INST},
    {.kind = TYPE_ERROR, .rg_name = "<error>", .c_name = "/* error */", .inst = &TYPE_ERROR_INST},
};

static const int32_t TYPE_INFO_COUNT = (int32_t)(sizeof(TYPE_INFO) / sizeof(TYPE_INFO[0]));

// ---------------------------------------------------------------------------
// Type utility functions
// ---------------------------------------------------------------------------
const Type *type_from_name(const char *name) {
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
