#include "repr/types.h"

/** Type singletons - one per primitive TypeKind. */
const Type TYPE_BOOL_INST = {.kind = TYPE_BOOL};
const Type TYPE_I8_INST = {.kind = TYPE_I8};
const Type TYPE_I16_INST = {.kind = TYPE_I16};
const Type TYPE_I32_INST = {.kind = TYPE_I32};
const Type TYPE_I64_INST = {.kind = TYPE_I64};
const Type TYPE_I128_INST = {.kind = TYPE_I128};
const Type TYPE_U8_INST = {.kind = TYPE_U8};
const Type TYPE_U16_INST = {.kind = TYPE_U16};
const Type TYPE_U32_INST = {.kind = TYPE_U32};
const Type TYPE_U64_INST = {.kind = TYPE_U64};
const Type TYPE_U128_INST = {.kind = TYPE_U128};
const Type TYPE_ISIZE_INST = {.kind = TYPE_ISIZE};
const Type TYPE_USIZE_INST = {.kind = TYPE_USIZE};
const Type TYPE_F32_INST = {.kind = TYPE_F32};
const Type TYPE_F64_INST = {.kind = TYPE_F64};
const Type TYPE_CHAR_INST = {.kind = TYPE_CHAR};
const Type TYPE_STR_INST = {.kind = TYPE_STR};
const Type TYPE_UNIT_INST = {.kind = TYPE_UNIT};
const Type TYPE_NEVER_INST = {.kind = TYPE_NEVER};
const Type TYPE_ERR_INST = {.kind = TYPE_ERR};

// Type classification flags.
enum {
    TF_INTEGER = 1 << 0,
    TF_SIGNED = 1 << 1,
    TF_FLOAT = 1 << 2,
    TF_PRINTABLE = 1 << 3,
    TF_STR_CONV = 1 << 4, // has rsg_str_from_TYPE runtime conversion
};

/**
 * Shared metadata table idxed by TypeKind: maps each kind to its
 * Resurg name, C name, runtime suffix, singleton ptr, and classification
 * flags.  Compound types (TYPE_ARRAY, TYPE_TUPLE) have zero-initialised
 * entries (rsg_name == NULL).
 */
typedef struct {
    const char *rsg_name;
    const char *c_name;
    const char *runtime_suffix; // for rsgu_print_TYPE, rsg_str_from_TYPE
    const Type *inst;
    uint8_t flags;
} TypeInfoEntry;

static const TypeInfoEntry TYPE_INFO[] = {
    [TYPE_BOOL] = {"bool", "bool", "bool", &TYPE_BOOL_INST, TF_PRINTABLE | TF_STR_CONV},
    [TYPE_I8] = {"i8", "int8_t", "i8", &TYPE_I8_INST, TF_INTEGER | TF_SIGNED},
    [TYPE_I16] = {"i16", "int16_t", "i16", &TYPE_I16_INST, TF_INTEGER | TF_SIGNED},
    [TYPE_I32] = {"i32", "int32_t", "i32", &TYPE_I32_INST,
                  TF_INTEGER | TF_SIGNED | TF_PRINTABLE | TF_STR_CONV},
    [TYPE_I64] = {"i64", "int64_t", "i64", &TYPE_I64_INST, TF_INTEGER | TF_SIGNED | TF_STR_CONV},
    [TYPE_I128] = {"i128", "__int128", "i128", &TYPE_I128_INST, TF_INTEGER | TF_SIGNED},
    [TYPE_U8] = {"u8", "uint8_t", "u8", &TYPE_U8_INST, TF_INTEGER},
    [TYPE_U16] = {"u16", "uint16_t", "u16", &TYPE_U16_INST, TF_INTEGER},
    [TYPE_U32] = {"u32", "uint32_t", "u32", &TYPE_U32_INST,
                  TF_INTEGER | TF_PRINTABLE | TF_STR_CONV},
    [TYPE_U64] = {"u64", "uint64_t", "u64", &TYPE_U64_INST, TF_INTEGER | TF_STR_CONV},
    [TYPE_U128] = {"u128", "unsigned __int128", "u128", &TYPE_U128_INST, TF_INTEGER},
    [TYPE_ISIZE] = {"isize", "intptr_t", "isize", &TYPE_ISIZE_INST, TF_INTEGER | TF_SIGNED},
    [TYPE_USIZE] = {"usize", "size_t", "usize", &TYPE_USIZE_INST, TF_INTEGER},
    [TYPE_F32] = {"f32", "float", "f32", &TYPE_F32_INST, TF_FLOAT | TF_STR_CONV},
    [TYPE_F64] = {"f64", "double", "f64", &TYPE_F64_INST, TF_FLOAT | TF_PRINTABLE | TF_STR_CONV},
    [TYPE_CHAR] = {"char", "uint32_t", "char", &TYPE_CHAR_INST, TF_PRINTABLE | TF_STR_CONV},
    [TYPE_STR] = {"str", "RsgStr", "str", &TYPE_STR_INST, TF_PRINTABLE},
    [TYPE_UNIT] = {"unit", "void", NULL, &TYPE_UNIT_INST, 0},
    [TYPE_NEVER] = {"never", "void", NULL, &TYPE_NEVER_INST, 0},
    [TYPE_ERR] = {"<err>", "/* err */", NULL, &TYPE_ERR_INST, 0},
    [TYPE_FN] = {NULL, "RsgFn", NULL, NULL, 0},
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

// Type utility fns.

const Type *type_from_name(const char *name) {
    if (name == NULL) {
        return NULL;
    }
    for (int32_t i = 0; i < TYPE_INFO_COUNT; i++) {
        if (TYPE_INFO[i].rsg_name != NULL && i != TYPE_ERR &&
            strcmp(name, TYPE_INFO[i].rsg_name) == 0) {
            return TYPE_INFO[i].inst;
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
        return arena_sprintf(arena, "[%d]%s", type->array.size, type_name(arena, type->array.elem));
    }
    if (type->kind == TYPE_SLICE) {
        return arena_sprintf(arena, "[]%s", type_name(arena, type->slice.elem));
    }
    if (type->kind == TYPE_TUPLE) {
        const char *result = "(";
        for (int32_t i = 0; i < type->tuple.count; i++) {
            if (i > 0) {
                result =
                    arena_sprintf(arena, "%s, %s", result, type_name(arena, type->tuple.elems[i]));
            } else {
                result =
                    arena_sprintf(arena, "%s%s", result, type_name(arena, type->tuple.elems[i]));
            }
        }
        return arena_sprintf(arena, "%s)", result);
    }
    if (type->kind == TYPE_STRUCT) {
        return type->struct_type.name;
    }
    if (type->kind == TYPE_PTR) {
        return arena_sprintf(arena, "*%s", type_name(arena, type->ptr.pointee));
    }
    if (type->kind == TYPE_ENUM) {
        return type->enum_type.name;
    }
    if (type->kind == TYPE_FN) {
        const char *prefix = "fn(";
        if (type->fn_type.fn_kind == FN_CLOSURE) {
            prefix = "Fn(";
        } else if (type->fn_type.fn_kind == FN_CLOSURE_MUT) {
            prefix = "FnMut(";
        }
        const char *result = prefix;
        for (int32_t i = 0; i < type->fn_type.param_count; i++) {
            if (i > 0) {
                result = arena_sprintf(arena, "%s, %s", result,
                                       type_name(arena, type->fn_type.params[i]));
            } else {
                result =
                    arena_sprintf(arena, "%s%s", result, type_name(arena, type->fn_type.params[i]));
            }
        }
        if (type_equal(type->fn_type.return_type, &TYPE_UNIT_INST)) {
            return arena_sprintf(arena, "%s)", result);
        }
        return arena_sprintf(arena, "%s) -> %s", result,
                             type_name(arena, type->fn_type.return_type));
    }
    if (type->kind == TYPE_MODULE) {
        return type->module_type.name;
    }
    if (type->kind == TYPE_COMPTIME_INT) {
        return arena_sprintf(arena, "%ld", (long)type->comptime_int.value);
    }
    return "<unknown>";
}

const char *c_type_str(const Type *type) {
    if (type == NULL) {
        return "/* ? */";
    }
    if (type->kind == TYPE_FN) {
        return "RsgFn";
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
        return a->array.size == b->array.size && type_equal(a->array.elem, b->array.elem);
    }
    if (a->kind == TYPE_SLICE) {
        return type_equal(a->slice.elem, b->slice.elem);
    }
    if (a->kind == TYPE_TUPLE) {
        if (a->tuple.count != b->tuple.count) {
            return false;
        }
        for (int32_t i = 0; i < a->tuple.count; i++) {
            if (!type_equal(a->tuple.elems[i], b->tuple.elems[i])) {
                return false;
            }
        }
        return true;
    }
    if (a->kind == TYPE_STRUCT) {
        return strcmp(a->struct_type.name, b->struct_type.name) == 0;
    }
    if (a->kind == TYPE_PTR) {
        return type_equal(a->ptr.pointee, b->ptr.pointee);
    }
    if (a->kind == TYPE_ENUM) {
        return strcmp(a->enum_type.name, b->enum_type.name) == 0;
    }
    if (a->kind == TYPE_FN) {
        if (a->fn_type.fn_kind != b->fn_type.fn_kind) {
            return false;
        }
        if (a->fn_type.param_count != b->fn_type.param_count) {
            return false;
        }
        for (int32_t i = 0; i < a->fn_type.param_count; i++) {
            if (!type_equal(a->fn_type.params[i], b->fn_type.params[i])) {
                return false;
            }
        }
        return type_equal(a->fn_type.return_type, b->fn_type.return_type);
    }
    if (a->kind == TYPE_MODULE) {
        return strcmp(a->module_type.name, b->module_type.name) == 0;
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

bool type_is_printable(const Type *type) {
    return type != NULL && (type_flags(type->kind) & TF_PRINTABLE);
}

const char *type_runtime_suffix(const Type *type) {
    if (type == NULL) {
        return NULL;
    }
    const TypeInfoEntry *info = type_info_lookup(type->kind);
    return (info != NULL) ? info->runtime_suffix : NULL;
}

bool type_is_str_convertible(const Type *type) {
    return type != NULL && (type_flags(type->kind) & TF_STR_CONV);
}

const Type *type_singleton(TypeKind kind) {
    const TypeInfoEntry *info = type_info_lookup(kind);
    return info != NULL ? info->inst : &TYPE_ERR_INST;
}

// ── Type accessors ─────────────────────────────────────────────────────

const Type *type_array_elem(const Type *type) {
    assert(type != NULL && type->kind == TYPE_ARRAY);
    return type->array.elem;
}

int32_t type_array_size(const Type *type) {
    assert(type != NULL && type->kind == TYPE_ARRAY);
    return type->array.size;
}

const Type **type_tuple_elems(const Type *type) {
    assert(type != NULL && type->kind == TYPE_TUPLE);
    return type->tuple.elems;
}

int32_t type_tuple_count(const Type *type) {
    assert(type != NULL && type->kind == TYPE_TUPLE);
    return type->tuple.count;
}

// ── Type constructors ──────────────────────────────────────────────────

static Type *type_create(Arena *arena, TypeKind kind) {
    Type *type = arena_alloc_zero(arena, sizeof(Type));
    type->kind = kind;
    return type;
}

Type *type_create_array(Arena *arena, const Type *elem, int32_t size) {
    Type *type = type_create(arena, TYPE_ARRAY);
    type->array.elem = elem;
    type->array.size = size;
    return type;
}

Type *type_create_slice(Arena *arena, const Type *elem) {
    Type *type = type_create(arena, TYPE_SLICE);
    type->slice.elem = elem;
    return type;
}

const Type *type_slice_elem(const Type *type) {
    assert(type != NULL && type->kind == TYPE_SLICE);
    return type->slice.elem;
}

Type *type_create_tuple(Arena *arena, const Type **elems, int32_t count) {
    Type *type = type_create(arena, TYPE_TUPLE);
    type->tuple.elems = elems;
    type->tuple.count = count;
    return type;
}

Type *type_create_struct(Arena *arena, const StructTypeSpec *spec) {
    Type *type = type_create(arena, TYPE_STRUCT);
    type->struct_type.name = spec->name;
    type->struct_type.fields = spec->fields;
    type->struct_type.field_count = spec->field_count;
    type->struct_type.embedded = spec->embedded;
    type->struct_type.embed_count = spec->embed_count;
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

Type *type_create_ptr(Arena *arena, const Type *pointee, bool is_mut) {
    Type *type = type_create(arena, TYPE_PTR);
    type->ptr.pointee = pointee;
    type->ptr.is_mut = is_mut;
    return type;
}

const Type *type_ptr_pointee(const Type *type) {
    assert(type != NULL && type->kind == TYPE_PTR);
    return type->ptr.pointee;
}

bool type_ptr_is_mut(const Type *type) {
    assert(type != NULL && type->kind == TYPE_PTR);
    return type->ptr.is_mut;
}

Type *type_create_enum(Arena *arena, const char *name, EnumVariant *variants,
                       int32_t variant_count) {
    Type *type = type_create(arena, TYPE_ENUM);
    type->enum_type.name = name;
    type->enum_type.variants = variants;
    type->enum_type.variant_count = variant_count;
    return type;
}

const char *type_enum_name(const Type *type) {
    assert(type != NULL && type->kind == TYPE_ENUM);
    return type->enum_type.name;
}

const EnumVariant *type_enum_variants(const Type *type) {
    assert(type != NULL && type->kind == TYPE_ENUM);
    return type->enum_type.variants;
}

int32_t type_enum_variant_count(const Type *type) {
    assert(type != NULL && type->kind == TYPE_ENUM);
    return type->enum_type.variant_count;
}

const EnumVariant *type_enum_find_variant(const Type *type, const char *name) {
    assert(type != NULL && type->kind == TYPE_ENUM);
    for (int32_t i = 0; i < type->enum_type.variant_count; i++) {
        if (strcmp(type->enum_type.variants[i].name, name) == 0) {
            return &type->enum_type.variants[i];
        }
    }
    return NULL;
}

Type *type_create_fn(Arena *arena, const FnTypeSpec *spec) {
    Type *type = type_create(arena, TYPE_FN);
    type->fn_type.params = spec->params;
    type->fn_type.param_count = spec->param_count;
    type->fn_type.return_type = spec->return_type;
    type->fn_type.fn_kind = spec->fn_kind;
    return type;
}

const Type **type_fn_params(const Type *type) {
    assert(type != NULL && type->kind == TYPE_FN);
    return type->fn_type.params;
}

int32_t type_fn_param_count(const Type *type) {
    assert(type != NULL && type->kind == TYPE_FN);
    return type->fn_type.param_count;
}

const Type *type_fn_return_type(const Type *type) {
    assert(type != NULL && type->kind == TYPE_FN);
    return type->fn_type.return_type;
}

FnTypeKind type_fn_kind(const Type *type) {
    assert(type != NULL && type->kind == TYPE_FN);
    return type->fn_type.fn_kind;
}

bool type_assignable(const Type *from, const Type *to) {
    if (type_equal(from, to)) {
        return true;
    }
    // fn subtyping: fn ⊆ Fn ⊆ FnMut
    if (from != NULL && to != NULL && from->kind == TYPE_FN && to->kind == TYPE_FN) {
        if (from->fn_type.param_count != to->fn_type.param_count) {
            return false;
        }
        for (int32_t i = 0; i < from->fn_type.param_count; i++) {
            if (!type_equal(from->fn_type.params[i], to->fn_type.params[i])) {
                return false;
            }
        }
        if (!type_equal(from->fn_type.return_type, to->fn_type.return_type)) {
            return false;
        }
        return from->fn_type.fn_kind <= to->fn_type.fn_kind;
    }
    return false;
}

Type *type_create_module(Arena *arena, const char *name) {
    Type *type = type_create(arena, TYPE_MODULE);
    type->module_type.name = name;
    return type;
}

Type *type_create_comptime_int(Arena *arena, int64_t value) {
    Type *type = type_create(arena, TYPE_COMPTIME_INT);
    type->comptime_int.value = value;
    return type;
}
