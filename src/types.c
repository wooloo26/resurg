#include "types.h"

/** Type singletons - one per primitive TypeKind. */
const Type TYPE_BOOL_INSTANCE = {.kind = TYPE_BOOL};
const Type TYPE_I8_INSTANCE = {.kind = TYPE_I8};
const Type TYPE_I16_INSTANCE = {.kind = TYPE_I16};
const Type TYPE_I32_INSTANCE = {.kind = TYPE_I32};
const Type TYPE_I64_INSTANCE = {.kind = TYPE_I64};
const Type TYPE_I128_INSTANCE = {.kind = TYPE_I128};
const Type TYPE_U8_INSTANCE = {.kind = TYPE_U8};
const Type TYPE_U16_INSTANCE = {.kind = TYPE_U16};
const Type TYPE_U32_INSTANCE = {.kind = TYPE_U32};
const Type TYPE_U64_INSTANCE = {.kind = TYPE_U64};
const Type TYPE_U128_INSTANCE = {.kind = TYPE_U128};
const Type TYPE_ISIZE_INSTANCE = {.kind = TYPE_ISIZE};
const Type TYPE_USIZE_INSTANCE = {.kind = TYPE_USIZE};
const Type TYPE_F32_INSTANCE = {.kind = TYPE_F32};
const Type TYPE_F64_INSTANCE = {.kind = TYPE_F64};
const Type TYPE_CHAR_INSTANCE = {.kind = TYPE_CHAR};
const Type TYPE_STRING_INSTANCE = {.kind = TYPE_STRING};
const Type TYPE_UNIT_INSTANCE = {.kind = TYPE_UNIT};
const Type TYPE_ERROR_INSTANCE = {.kind = TYPE_ERROR};

/**
 * Shared metadata table: maps each TypeKind to its Resurg name, C name,
 * and singleton pointer.
 */
static const struct {
    TypeKind kind;
    const char *rsg_name;
    const char *c_name;
    const Type *instance;
} TYPE_INFO[] = {
    {.kind = TYPE_BOOL, .rsg_name = "bool", .c_name = "bool", .instance = &TYPE_BOOL_INSTANCE},
    {.kind = TYPE_I8, .rsg_name = "i8", .c_name = "int8_t", .instance = &TYPE_I8_INSTANCE},
    {.kind = TYPE_I16, .rsg_name = "i16", .c_name = "int16_t", .instance = &TYPE_I16_INSTANCE},
    {.kind = TYPE_I32, .rsg_name = "i32", .c_name = "int32_t", .instance = &TYPE_I32_INSTANCE},
    {.kind = TYPE_I64, .rsg_name = "i64", .c_name = "int64_t", .instance = &TYPE_I64_INSTANCE},
    {.kind = TYPE_I128, .rsg_name = "i128", .c_name = "__int128", .instance = &TYPE_I128_INSTANCE},
    {.kind = TYPE_U8, .rsg_name = "u8", .c_name = "uint8_t", .instance = &TYPE_U8_INSTANCE},
    {.kind = TYPE_U16, .rsg_name = "u16", .c_name = "uint16_t", .instance = &TYPE_U16_INSTANCE},
    {.kind = TYPE_U32, .rsg_name = "u32", .c_name = "uint32_t", .instance = &TYPE_U32_INSTANCE},
    {.kind = TYPE_U64, .rsg_name = "u64", .c_name = "uint64_t", .instance = &TYPE_U64_INSTANCE},
    {.kind = TYPE_U128, .rsg_name = "u128", .c_name = "unsigned __int128", .instance = &TYPE_U128_INSTANCE},
    {.kind = TYPE_ISIZE, .rsg_name = "isize", .c_name = "intptr_t", .instance = &TYPE_ISIZE_INSTANCE},
    {.kind = TYPE_USIZE, .rsg_name = "usize", .c_name = "size_t", .instance = &TYPE_USIZE_INSTANCE},
    {.kind = TYPE_F32, .rsg_name = "f32", .c_name = "float", .instance = &TYPE_F32_INSTANCE},
    {.kind = TYPE_F64, .rsg_name = "f64", .c_name = "double", .instance = &TYPE_F64_INSTANCE},
    {.kind = TYPE_CHAR, .rsg_name = "char", .c_name = "char", .instance = &TYPE_CHAR_INSTANCE},
    {.kind = TYPE_STRING, .rsg_name = "str", .c_name = "RsgString", .instance = &TYPE_STRING_INSTANCE},
    {.kind = TYPE_UNIT, .rsg_name = "unit", .c_name = "void", .instance = &TYPE_UNIT_INSTANCE},
    {.kind = TYPE_ERROR, .rsg_name = "<error>", .c_name = "/* error */", .instance = &TYPE_ERROR_INSTANCE},
};

static const int32_t TYPE_INFO_COUNT = (int32_t)(sizeof(TYPE_INFO) / sizeof(TYPE_INFO[0]));

// Type utility functions.

const Type *type_from_name(const char *name) {
    if (name == NULL) {
        return NULL;
    }
    for (int32_t i = 0; i < TYPE_INFO_COUNT; i++) {
        if (TYPE_INFO[i].kind != TYPE_ERROR && strcmp(name, TYPE_INFO[i].rsg_name) == 0) {
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
            return TYPE_INFO[i].rsg_name;
        }
    }
    if (type->kind == TYPE_ARRAY) {
        static char array_name_buffer[128];
        snprintf(array_name_buffer, sizeof(array_name_buffer), "[%d]%s", type->array_size,
                 type_name(type->array_element));
        return array_name_buffer;
    }
    if (type->kind == TYPE_TUPLE) {
        static char tuple_name_buffer[256];
        int32_t offset = snprintf(tuple_name_buffer, sizeof(tuple_name_buffer), "(");
        for (int32_t i = 0; i < type->tuple_count; i++) {
            if (i > 0) {
                offset += snprintf(tuple_name_buffer + offset, sizeof(tuple_name_buffer) - offset, ", ");
            }
            offset += snprintf(tuple_name_buffer + offset, sizeof(tuple_name_buffer) - offset, "%s",
                               type_name(type->tuple_elements[i]));
        }
        snprintf(tuple_name_buffer + offset, sizeof(tuple_name_buffer) - offset, ")");
        return tuple_name_buffer;
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
    if (a->kind != b->kind) {
        return false;
    }
    if (a->kind == TYPE_ARRAY) {
        return a->array_size == b->array_size && type_equal(a->array_element, b->array_element);
    }
    if (a->kind == TYPE_TUPLE) {
        if (a->tuple_count != b->tuple_count) {
            return false;
        }
        for (int32_t i = 0; i < a->tuple_count; i++) {
            if (!type_equal(a->tuple_elements[i], b->tuple_elements[i])) {
                return false;
            }
        }
        return true;
    }
    return true;
}

bool type_is_numeric(const Type *type) {
    return type != NULL && (type_is_integer(type) || type_is_float(type));
}

bool type_is_integer(const Type *type) {
    if (type == NULL) {
        return false;
    }
    switch (type->kind) {
    case TYPE_I8:
    case TYPE_I16:
    case TYPE_I32:
    case TYPE_I64:
    case TYPE_I128:
    case TYPE_U8:
    case TYPE_U16:
    case TYPE_U32:
    case TYPE_U64:
    case TYPE_U128:
    case TYPE_ISIZE:
    case TYPE_USIZE:
        return true;
    default:
        return false;
    }
}

bool type_is_signed_integer(const Type *type) {
    if (type == NULL) {
        return false;
    }
    switch (type->kind) {
    case TYPE_I8:
    case TYPE_I16:
    case TYPE_I32:
    case TYPE_I64:
    case TYPE_I128:
    case TYPE_ISIZE:
        return true;
    default:
        return false;
    }
}

bool type_is_unsigned_integer(const Type *type) {
    if (type == NULL) {
        return false;
    }
    switch (type->kind) {
    case TYPE_U8:
    case TYPE_U16:
    case TYPE_U32:
    case TYPE_U64:
    case TYPE_U128:
    case TYPE_USIZE:
        return true;
    default:
        return false;
    }
}

bool type_is_float(const Type *type) {
    return type != NULL && (type->kind == TYPE_F32 || type->kind == TYPE_F64);
}

Type *type_create_array(Arena *arena, const Type *element, int32_t size) {
    Type *type = arena_alloc(arena, sizeof(Type));
    memset(type, 0, sizeof(Type));
    type->kind = TYPE_ARRAY;
    type->array_element = element;
    type->array_size = size;
    return type;
}

Type *type_create_tuple(Arena *arena, const Type **elements, int32_t count) {
    Type *type = arena_alloc(arena, sizeof(Type));
    memset(type, 0, sizeof(Type));
    type->kind = TYPE_TUPLE;
    type->tuple_elements = elements;
    type->tuple_count = count;
    return type;
}
