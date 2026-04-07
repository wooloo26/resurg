#ifndef RSG_TYPES_H
#define RSG_TYPES_H

#include "core/common.h"

/**
 * @file types.h
 * @brief Resolved types produced by semantic analysis.  Primitive types use
 * singleton insts; compound types (arrays, tuples) are arena-allocated.
 */
typedef enum {
    TYPE_BOOL,
    TYPE_I8,
    TYPE_I16,
    TYPE_I32,
    TYPE_I64,
    TYPE_I128,
    TYPE_U8,
    TYPE_U16,
    TYPE_U32,
    TYPE_U64,
    TYPE_U128,
    TYPE_ISIZE,
    TYPE_USIZE,
    TYPE_F32,
    TYPE_F64,
    TYPE_CHAR,
    TYPE_STR,
    TYPE_UNIT,
    TYPE_NEVER,  // bottom type (never completes)
    TYPE_ARRAY,  // [N]T
    TYPE_SLICE,  // []T
    TYPE_TUPLE,  // (A, B, ...)
    TYPE_STRUCT, // struct { fields }
    TYPE_PTR,    // *T
    TYPE_ENUM,   // enum { variants }
    TYPE_FN,     // fn(Params) -> Return
    TYPE_ERR,    // sentinel for continued checking after type errs
} TypeKind;

/** Distinguishes fn/Fn/FnMut function type kinds. */
typedef enum {
    FN_PLAIN,       // fn(P) -> R  — plain function type
    FN_CLOSURE,     // Fn(P) -> R  — closure, readonly captures
    FN_CLOSURE_MUT, // FnMut(P) -> R — closure, mutable captures
} FnTypeKind;

typedef struct Type Type;

/** A field in a struct type. */
typedef struct {
    const char *name;
    const Type *type;
} StructField;

/** Variant kind in an enum type. */
typedef enum {
    ENUM_VARIANT_UNIT,
    ENUM_VARIANT_TUPLE,
    ENUM_VARIANT_STRUCT,
} EnumVariantKind;

/** A variant in an enum type. */
typedef struct {
    const char *name;
    EnumVariantKind kind;
    const Type **tuple_types; // for TUPLE variant (stretchy buf)
    int32_t tuple_count;
    StructField *fields; // for STRUCT variant (stretchy buf)
    int32_t field_count;
    int32_t discriminant;
} EnumVariant;

struct Type {
    TypeKind kind;
    union {
        struct {
            const Type *elem;
            int32_t size;
        } array;
        struct {
            const Type **elems;
            int32_t count;
        } tuple;
        struct {
            const char *name;
            StructField *fields;
            int32_t field_count;
            const Type **embedded;
            int32_t embed_count;
        } struct_type;
        struct {
            const Type *elem;
        } slice;
        struct {
            const Type *pointee;
            bool is_mut;
        } ptr;
        struct {
            const char *name;
            EnumVariant *variants;
            int32_t variant_count;
        } enum_type;
        struct {
            const Type **params;
            int32_t param_count;
            const Type *return_type;
            FnTypeKind fn_kind;
        } fn_type;
    };
};

// ── Type accessors (never access union members directly) ───────────────

/** Return the elem type of an array type.  Asserts kind == TYPE_ARRAY. */
const Type *type_array_elem(const Type *type);
/** Return the fixed size of an array type.  Asserts kind == TYPE_ARRAY. */
int32_t type_array_size(const Type *type);
/** Return the elem types of a tuple type.  Asserts kind == TYPE_TUPLE. */
const Type **type_tuple_elems(const Type *type);
/** Return the elem count of a tuple type.  Asserts kind == TYPE_TUPLE. */
int32_t type_tuple_count(const Type *type);

/** Singleton type insts (avoids heap alloc for primitives). */
extern const Type TYPE_BOOL_INST;
extern const Type TYPE_I8_INST;
extern const Type TYPE_I16_INST;
extern const Type TYPE_I32_INST;
extern const Type TYPE_I64_INST;
extern const Type TYPE_I128_INST;
extern const Type TYPE_U8_INST;
extern const Type TYPE_U16_INST;
extern const Type TYPE_U32_INST;
extern const Type TYPE_U64_INST;
extern const Type TYPE_U128_INST;
extern const Type TYPE_ISIZE_INST;
extern const Type TYPE_USIZE_INST;
extern const Type TYPE_F32_INST;
extern const Type TYPE_F64_INST;
extern const Type TYPE_CHAR_INST;
extern const Type TYPE_STR_INST;
extern const Type TYPE_UNIT_INST;
extern const Type TYPE_NEVER_INST;
extern const Type TYPE_ERR_INST;

/**
 * Resolve a Resurg type name ("i32", "str", ...) to its singleton.  Returns
 * NULL if @p name is unknown.
 */
const Type *type_from_name(const char *name);
/** Return the Resurg-language name for @p type (e.g. "i32"). */
const char *type_name(Arena *arena, const Type *type);
/** Return the C type str used during code generation (e.g. "int32_t"). */
const char *c_type_str(const Type *type);
/** Return true if both types are non-NULL and structurally equal. */
bool type_equal(const Type *a, const Type *b);
/** Return true if @p type is any numeric type (integer or float). */
bool type_is_numeric(const Type *type);
/** Return true if @p type is any integer type. */
bool type_is_integer(const Type *type);
/** Return true if @p type is any signed integer type. */
bool type_is_signed_integer(const Type *type);
/** Return true if @p type is any unsigned integer type. */
bool type_is_unsigned_integer(const Type *type);
/** Return true if @p type is any floating-point type. */
bool type_is_float(const Type *type);

/** Return the singleton inst for a primitive @p kind, or TYPE_ERR for compounds. */
const Type *type_singleton(TypeKind kind);

/** Create an array type [size]elem. */
Type *type_create_array(Arena *arena, const Type *elem, int32_t size);
/** Create a slice type []elem. */
Type *type_create_slice(Arena *arena, const Type *elem);
/** Return the elem type of a slice type.  Asserts kind == TYPE_SLICE. */
const Type *type_slice_elem(const Type *type);
/** Create a tuple type from an array of elem types. */
Type *type_create_tuple(Arena *arena, const Type **elems, int32_t count);

/** Params for creating a struct type. */
typedef struct {
    const char *name;
    StructField *fields;
    int32_t field_count;
    const Type **embedded;
    int32_t embed_count;
} StructTypeSpec;

/** Create a struct type with the given spec. */
Type *type_create_struct(Arena *arena, const StructTypeSpec *spec);

/** Return the struct name.  Asserts kind == TYPE_STRUCT. */
const char *type_struct_name(const Type *type);
/** Return the fields of a struct type.  Asserts kind == TYPE_STRUCT. */
const StructField *type_struct_fields(const Type *type);
/** Return the field count of a struct type.  Asserts kind == TYPE_STRUCT. */
int32_t type_struct_field_count(const Type *type);
/** Look up a field by name in a struct type.  Returns NULL if not found. */
const StructField *type_struct_find_field(const Type *type, const char *name);

/** Create a ptr type *pointee. */
Type *type_create_ptr(Arena *arena, const Type *pointee, bool is_mut);
/** Return the pointee type of a ptr.  Asserts kind == TYPE_PTR. */
const Type *type_ptr_pointee(const Type *type);
/** Return whether a ptr type is mutable.  Asserts kind == TYPE_PTR. */
bool type_ptr_is_mut(const Type *type);

/** Create an enum type with the given variants. */
Type *type_create_enum(Arena *arena, const char *name, EnumVariant *variants,
                       int32_t variant_count);
/** Return the enum name.  Asserts kind == TYPE_ENUM. */
const char *type_enum_name(const Type *type);
/** Return the variants array.  Asserts kind == TYPE_ENUM. */
const EnumVariant *type_enum_variants(const Type *type);
/** Return the variant count.  Asserts kind == TYPE_ENUM. */
int32_t type_enum_variant_count(const Type *type);
/** Find a variant by name.  Returns NULL if not found. */
const EnumVariant *type_enum_find_variant(const Type *type, const char *name);

/** Create a function type fn/Fn/FnMut(params) -> return_type. */
Type *type_create_fn(Arena *arena, const Type **params, int32_t param_count,
                     const Type *return_type, FnTypeKind fn_kind);
/** Return the param types of a fn type.  Asserts kind == TYPE_FN. */
const Type **type_fn_params(const Type *type);
/** Return the param count of a fn type.  Asserts kind == TYPE_FN. */
int32_t type_fn_param_count(const Type *type);
/** Return the return type of a fn type.  Asserts kind == TYPE_FN. */
const Type *type_fn_return_type(const Type *type);
/** Return the fn kind (FN_PLAIN, FN_CLOSURE, FN_CLOSURE_MUT). */
FnTypeKind type_fn_kind(const Type *type);
/** Return true if @p from is assignable to @p to (includes fn subtyping). */
bool type_assignable(const Type *from, const Type *to);

#endif // RSG_TYPES_H
