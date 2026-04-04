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

// Type classification flags.
enum {
    TF_INTEGER = 1 << 0,
    TF_SIGNED = 1 << 1,
    TF_FLOAT = 1 << 2,
};

/**
 * Shared metadata table indexed by TypeKind: maps each kind to its
 * Resurg name, C name, singleton pointer, and classification flags.
 * Compound types (TYPE_ARRAY, TYPE_TUPLE) have zero-initialised
 * entries (rsg_name == NULL).
 */
typedef struct {
    const char *rsg_name;
    const char *c_name;
    const Type *instance;
    uint8_t flags;
} TypeInfoEntry;

static const TypeInfoEntry TYPE_INFO[] = {
    [TYPE_BOOL] = {"bool", "bool", &TYPE_BOOL_INSTANCE, 0},
    [TYPE_I8] = {"i8", "int8_t", &TYPE_I8_INSTANCE, TF_INTEGER | TF_SIGNED},
    [TYPE_I16] = {"i16", "int16_t", &TYPE_I16_INSTANCE, TF_INTEGER | TF_SIGNED},
    [TYPE_I32] = {"i32", "int32_t", &TYPE_I32_INSTANCE, TF_INTEGER | TF_SIGNED},
    [TYPE_I64] = {"i64", "int64_t", &TYPE_I64_INSTANCE, TF_INTEGER | TF_SIGNED},
    [TYPE_I128] = {"i128", "__int128", &TYPE_I128_INSTANCE, TF_INTEGER | TF_SIGNED},
    [TYPE_U8] = {"u8", "uint8_t", &TYPE_U8_INSTANCE, TF_INTEGER},
    [TYPE_U16] = {"u16", "uint16_t", &TYPE_U16_INSTANCE, TF_INTEGER},
    [TYPE_U32] = {"u32", "uint32_t", &TYPE_U32_INSTANCE, TF_INTEGER},
    [TYPE_U64] = {"u64", "uint64_t", &TYPE_U64_INSTANCE, TF_INTEGER},
    [TYPE_U128] = {"u128", "unsigned __int128", &TYPE_U128_INSTANCE, TF_INTEGER},
    [TYPE_ISIZE] = {"isize", "intptr_t", &TYPE_ISIZE_INSTANCE, TF_INTEGER | TF_SIGNED},
    [TYPE_USIZE] = {"usize", "size_t", &TYPE_USIZE_INSTANCE, TF_INTEGER},
    [TYPE_F32] = {"f32", "float", &TYPE_F32_INSTANCE, TF_FLOAT},
    [TYPE_F64] = {"f64", "double", &TYPE_F64_INSTANCE, TF_FLOAT},
    [TYPE_CHAR] = {"char", "uint32_t", &TYPE_CHAR_INSTANCE, 0},
    [TYPE_STRING] = {"str", "RsgString", &TYPE_STRING_INSTANCE, 0},
    [TYPE_UNIT] = {"unit", "void", &TYPE_UNIT_INSTANCE, 0},
    [TYPE_ERROR] = {"<error>", "/* error */", &TYPE_ERROR_INSTANCE, 0},
};

static const int32_t TYPE_INFO_COUNT = (int32_t)(sizeof(TYPE_INFO) / sizeof(TYPE_INFO[0]));

/**
 * Find the TYPE_INFO entry for @p kind.  Returns NULL for compound
 * types (TYPE_ARRAY, TYPE_TUPLE) that have no table entry.
 */
static const TypeInfoEntry *find_type_info(TypeKind kind) {
    if (kind < 0 || kind >= TYPE_INFO_COUNT || TYPE_INFO[kind].rsg_name == NULL) {
        return NULL;
    }
    return &TYPE_INFO[kind];
}

static uint8_t type_flags(TypeKind kind) {
    return (kind >= 0 && kind < TYPE_INFO_COUNT) ? TYPE_INFO[kind].flags : 0;
}

// Type utility functions.

const Type *type_from_name(const char *name) {
    if (name == NULL) {
        return NULL;
    }
    for (int32_t i = 0; i < TYPE_INFO_COUNT; i++) {
        if (TYPE_INFO[i].rsg_name != NULL && i != TYPE_ERROR &&
            strcmp(name, TYPE_INFO[i].rsg_name) == 0) {
            return TYPE_INFO[i].instance;
        }
    }
    return NULL;
}

const char *type_name(Arena *arena, const Type *type) {
    if (type == NULL) {
        return "<unknown>";
    }
    const TypeInfoEntry *info = find_type_info(type->kind);
    if (info != NULL) {
        return info->rsg_name;
    }
    if (type->kind == TYPE_ARRAY) {
        return arena_sprintf(arena, "[%d]%s", type->array_size,
                             type_name(arena, type->array_element));
    }
    if (type->kind == TYPE_TUPLE) {
        const char *result = "(";
        for (int32_t i = 0; i < type->tuple_count; i++) {
            if (i > 0) {
                result = arena_sprintf(arena, "%s, %s", result,
                                       type_name(arena, type->tuple_elements[i]));
            } else {
                result =
                    arena_sprintf(arena, "%s%s", result, type_name(arena, type->tuple_elements[i]));
            }
        }
        return arena_sprintf(arena, "%s)", result);
    }
    return "<unknown>";
}

const char *c_type_string(const Type *type) {
    if (type == NULL) {
        return "/* ? */";
    }
    const TypeInfoEntry *info = find_type_info(type->kind);
    return info != NULL ? info->c_name : "/* ? */";
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
    return type != NULL && (type_flags(type->kind) & (TF_INTEGER | TF_FLOAT));
}

bool type_is_integer(const Type *type) {
    return type != NULL && (type_flags(type->kind) & TF_INTEGER);
}

bool type_is_signed_integer(const Type *type) {
    return type != NULL &&
           (type_flags(type->kind) & (TF_INTEGER | TF_SIGNED)) == (TF_INTEGER | TF_SIGNED);
}

bool type_is_unsigned_integer(const Type *type) {
    return type != NULL && (type_flags(type->kind) & (TF_INTEGER | TF_SIGNED)) == TF_INTEGER;
}

bool type_is_float(const Type *type) {
    return type != NULL && (type_flags(type->kind) & TF_FLOAT);
}

const Type *type_singleton(TypeKind kind) {
    const TypeInfoEntry *info = find_type_info(kind);
    return info != NULL ? info->instance : &TYPE_ERROR_INSTANCE;
}

static Type *type_create(Arena *arena, TypeKind kind) {
    Type *type = arena_alloc(arena, sizeof(Type));
    memset(type, 0, sizeof(Type));
    type->kind = kind;
    return type;
}

Type *type_create_array(Arena *arena, const Type *element, int32_t size) {
    Type *type = type_create(arena, TYPE_ARRAY);
    type->array_element = element;
    type->array_size = size;
    return type;
}

Type *type_create_tuple(Arena *arena, const Type **elements, int32_t count) {
    Type *type = type_create(arena, TYPE_TUPLE);
    type->tuple_elements = elements;
    type->tuple_count = count;
    return type;
}
