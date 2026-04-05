#include "types/types.h"

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
static const TypeInfoEntry *type_info_lookup(TypeKind kind) {
    if ((int32_t)kind < 0 || (int32_t)kind >= TYPE_INFO_COUNT || TYPE_INFO[kind].rsg_name == NULL) {
        return NULL;
    }
    return &TYPE_INFO[kind];
}

static uint8_t type_flags(TypeKind kind) {
    return ((int32_t)kind >= 0 && (int32_t)kind < TYPE_INFO_COUNT) ? TYPE_INFO[kind].flags : 0;
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
    const TypeInfoEntry *info = type_info_lookup(type->kind);
    if (info != NULL) {
        return info->rsg_name;
    }
    if (type->kind == TYPE_ARRAY) {
        return arena_sprintf(arena, "[%d]%s", type->array.size,
                             type_name(arena, type->array.element));
    }
    if (type->kind == TYPE_TUPLE) {
        const char *result = "(";
        for (int32_t i = 0; i < type->tuple.count; i++) {
            if (i > 0) {
                result = arena_sprintf(arena, "%s, %s", result,
                                       type_name(arena, type->tuple.elements[i]));
            } else {
                result =
                    arena_sprintf(arena, "%s%s", result, type_name(arena, type->tuple.elements[i]));
            }
        }
        return arena_sprintf(arena, "%s)", result);
    }
    if (type->kind == TYPE_STRUCT) {
        return type->struct_type.name;
    }
    if (type->kind == TYPE_POINTER) {
        return arena_sprintf(arena, "*%s", type_name(arena, type->pointer.pointee));
    }
    return "<unknown>";
}

const char *c_type_string(const Type *type) {
    if (type == NULL) {
        return "/* ? */";
    }
    const TypeInfoEntry *info = type_info_lookup(type->kind);
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
        return a->array.size == b->array.size && type_equal(a->array.element, b->array.element);
    }
    if (a->kind == TYPE_TUPLE) {
        if (a->tuple.count != b->tuple.count) {
            return false;
        }
        for (int32_t i = 0; i < a->tuple.count; i++) {
            if (!type_equal(a->tuple.elements[i], b->tuple.elements[i])) {
                return false;
            }
        }
        return true;
    }
    if (a->kind == TYPE_STRUCT) {
        return strcmp(a->struct_type.name, b->struct_type.name) == 0;
    }
    if (a->kind == TYPE_POINTER) {
        return type_equal(a->pointer.pointee, b->pointer.pointee);
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
    const TypeInfoEntry *info = type_info_lookup(kind);
    return info != NULL ? info->instance : &TYPE_ERROR_INSTANCE;
}

// ── Type accessors ─────────────────────────────────────────────────────

const Type *type_array_element(const Type *type) {
    assert(type != NULL && type->kind == TYPE_ARRAY);
    return type->array.element;
}

int32_t type_array_size(const Type *type) {
    assert(type != NULL && type->kind == TYPE_ARRAY);
    return type->array.size;
}

const Type **type_tuple_elements(const Type *type) {
    assert(type != NULL && type->kind == TYPE_TUPLE);
    return type->tuple.elements;
}

int32_t type_tuple_count(const Type *type) {
    assert(type != NULL && type->kind == TYPE_TUPLE);
    return type->tuple.count;
}

// ── Type constructors ──────────────────────────────────────────────────

static Type *type_create(Arena *arena, TypeKind kind) {
    Type *type = arena_alloc(arena, sizeof(Type));
    memset(type, 0, sizeof(Type));
    type->kind = kind;
    return type;
}

Type *type_create_array(Arena *arena, const Type *element, int32_t size) {
    Type *type = type_create(arena, TYPE_ARRAY);
    type->array.element = element;
    type->array.size = size;
    return type;
}

Type *type_create_tuple(Arena *arena, const Type **elements, int32_t count) {
    Type *type = type_create(arena, TYPE_TUPLE);
    type->tuple.elements = elements;
    type->tuple.count = count;
    return type;
}

Type *type_create_struct(Arena *arena, const char *name, StructField *fields, int32_t field_count,
                         const Type **embedded, int32_t embed_count) {
    Type *type = type_create(arena, TYPE_STRUCT);
    type->struct_type.name = name;
    type->struct_type.fields = fields;
    type->struct_type.field_count = field_count;
    type->struct_type.embedded = embedded;
    type->struct_type.embed_count = embed_count;
    return type;
}

const char *type_struct_name(const Type *type) {
    assert(type != NULL && type->kind == TYPE_STRUCT);
    return type->struct_type.name;
}

const StructField *type_struct_fields(const Type *type) {
    assert(type != NULL && type->kind == TYPE_STRUCT);
    return type->struct_type.fields;
}

int32_t type_struct_field_count(const Type *type) {
    assert(type != NULL && type->kind == TYPE_STRUCT);
    return type->struct_type.field_count;
}

const StructField *type_struct_find_field(const Type *type, const char *name) {
    assert(type != NULL && type->kind == TYPE_STRUCT);
    for (int32_t i = 0; i < type->struct_type.field_count; i++) {
        if (strcmp(type->struct_type.fields[i].name, name) == 0) {
            return &type->struct_type.fields[i];
        }
    }
    return NULL;
}

Type *type_create_pointer(Arena *arena, const Type *pointee, bool is_mut) {
    Type *type = type_create(arena, TYPE_POINTER);
    type->pointer.pointee = pointee;
    type->pointer.is_mut = is_mut;
    return type;
}

const Type *type_pointer_pointee(const Type *type) {
    assert(type != NULL && type->kind == TYPE_POINTER);
    return type->pointer.pointee;
}

bool type_pointer_is_mut(const Type *type) {
    assert(type != NULL && type->kind == TYPE_POINTER);
    return type->pointer.is_mut;
}
