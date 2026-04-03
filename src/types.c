#include "types.h"

/** Type singletons — one per primitive TypeKind. */
const Type TYPE_BOOL_INSTANCE = {.kind = TYPE_BOOL};
const Type TYPE_I32_INSTANCE = {.kind = TYPE_I32};
const Type TYPE_U32_INSTANCE = {.kind = TYPE_U32};
const Type TYPE_F64_INSTANCE = {.kind = TYPE_F64};
const Type TYPE_STRING_INSTANCE = {.kind = TYPE_STRING};
const Type TYPE_UNIT_INSTANCE = {.kind = TYPE_UNIT};
const Type TYPE_ERROR_INSTANCE = {.kind = TYPE_ERROR};

/**
 * Shared metadata table: maps each TypeKind to its Resurg name, C name,
 * and singleton pointer.
 */
static const struct {
    TypeKind kind;
    const char *resurg_name;
    const char *c_name;
    const Type *instance;
} TYPE_INFO[] = {
    {.kind = TYPE_BOOL, .resurg_name = "bool", .c_name = "bool", .instance = &TYPE_BOOL_INSTANCE},
    {.kind = TYPE_I32, .resurg_name = "i32", .c_name = "int32_t", .instance = &TYPE_I32_INSTANCE},
    {.kind = TYPE_U32, .resurg_name = "u32", .c_name = "uint32_t", .instance = &TYPE_U32_INSTANCE},
    {.kind = TYPE_F64, .resurg_name = "f64", .c_name = "double", .instance = &TYPE_F64_INSTANCE},
    {.kind = TYPE_STRING, .resurg_name = "str", .c_name = "RgString", .instance = &TYPE_STRING_INSTANCE},
    {.kind = TYPE_UNIT, .resurg_name = "unit", .c_name = "void", .instance = &TYPE_UNIT_INSTANCE},
    {.kind = TYPE_ERROR, .resurg_name = "<error>", .c_name = "/* error */", .instance = &TYPE_ERROR_INSTANCE},
};

static const int32_t TYPE_INFO_COUNT = (int32_t)(sizeof(TYPE_INFO) / sizeof(TYPE_INFO[0]));

// Type utility functions.

const Type *type_from_name(const char *name) {
    if (name == NULL) {
        return NULL;
    }
    for (int32_t i = 0; i < TYPE_INFO_COUNT; i++) {
        if (TYPE_INFO[i].kind != TYPE_ERROR && strcmp(name, TYPE_INFO[i].resurg_name) == 0) {
            return TYPE_INFO[i].instance;
        }
    }
    return NULL;
}

const char *type_name(const Type *type) {
    if (type == NULL) {
        return "<unknown>";
    }
    for (int32_t i = 0; i < TYPE_INFO_COUNT; i++) {
        if (TYPE_INFO[i].kind == type->kind) {
            return TYPE_INFO[i].resurg_name;
        }
    }
    return "<unknown>";
}

const char *c_type_string(const Type *type) {
    if (type == NULL) {
        return "/* ? */";
    }
    for (int32_t i = 0; i < TYPE_INFO_COUNT; i++) {
        if (TYPE_INFO[i].kind == type->kind) {
            return TYPE_INFO[i].c_name;
        }
    }
    return "/* ? */";
}

bool type_equal(const Type *a, const Type *b) {
    if (a == NULL || b == NULL) {
        return false;
    }
    return a->kind == b->kind;
}

bool type_is_numeric(const Type *type) {
    return type != NULL && (type->kind == TYPE_I32 || type->kind == TYPE_U32 || type->kind == TYPE_F64);
}

bool type_is_integer(const Type *type) {
    return type != NULL && (type->kind == TYPE_I32 || type->kind == TYPE_U32);
}
