#ifndef RG_TYPES_H
#define RG_TYPES_H

#include "core/common.h"

/**
 * @file types.h
 * @brief Resolved types produced by semantic analysis.  Primitive types use
 * singleton instances; compound types (arrays, tuples) are arena-allocated.
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
    TYPE_STRING,
    TYPE_UNIT,
    TYPE_ARRAY,   // [N]T
    TYPE_TUPLE,   // (A, B, ...)
    TYPE_STRUCT,  // struct { fields }
    TYPE_POINTER, // *T
    TYPE_ENUM,    // enum { variants }
    TYPE_ERROR,   // sentinel for continued checking after type errors
} TypeKind;

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
            const Type *element;
            int32_t size;
        } array;
        struct {
            const Type **elements;
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
            const Type *pointee;
            bool is_mut;
        } pointer;
        struct {
            const char *name;
            EnumVariant *variants;
            int32_t variant_count;
        } enum_type;
    };
};

// ── Type accessors (never access union members directly) ───────────────

/** Return the element type of an array type.  Asserts kind == TYPE_ARRAY. */
const Type *type_array_element(const Type *type);
/** Return the fixed size of an array type.  Asserts kind == TYPE_ARRAY. */
int32_t type_array_size(const Type *type);
/** Return the element types of a tuple type.  Asserts kind == TYPE_TUPLE. */
const Type **type_tuple_elements(const Type *type);
/** Return the element count of a tuple type.  Asserts kind == TYPE_TUPLE. */
int32_t type_tuple_count(const Type *type);

/** Singleton type instances (avoids heap allocation for primitives). */
extern const Type TYPE_BOOL_INSTANCE;
extern const Type TYPE_I8_INSTANCE;
extern const Type TYPE_I16_INSTANCE;
extern const Type TYPE_I32_INSTANCE;
extern const Type TYPE_I64_INSTANCE;
extern const Type TYPE_I128_INSTANCE;
extern const Type TYPE_U8_INSTANCE;
extern const Type TYPE_U16_INSTANCE;
extern const Type TYPE_U32_INSTANCE;
extern const Type TYPE_U64_INSTANCE;
extern const Type TYPE_U128_INSTANCE;
extern const Type TYPE_ISIZE_INSTANCE;
extern const Type TYPE_USIZE_INSTANCE;
extern const Type TYPE_F32_INSTANCE;
extern const Type TYPE_F64_INSTANCE;
extern const Type TYPE_CHAR_INSTANCE;
extern const Type TYPE_STRING_INSTANCE;
extern const Type TYPE_UNIT_INSTANCE;
extern const Type TYPE_ERROR_INSTANCE;

/**
 * Resolve a Resurg type name ("i32", "str", ...) to its singleton.  Returns
 * NULL if @p name is unknown.
 */
const Type *type_from_name(const char *name);
/** Return the Resurg-language name for @p type (e.g. "i32"). */
const char *type_name(Arena *arena, const Type *type);
/** Return the C type string used during code generation (e.g. "int32_t"). */
const char *c_type_string(const Type *type);
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

/** Return the singleton instance for a primitive @p kind, or TYPE_ERROR for compounds. */
const Type *type_singleton(TypeKind kind);

/** Create an array type [size]element. */
Type *type_create_array(Arena *arena, const Type *element, int32_t size);
/** Create a tuple type from an array of element types. */
Type *type_create_tuple(Arena *arena, const Type **elements, int32_t count);

/** Create a struct type with the given fields and embedded types. */
Type *type_create_struct(Arena *arena, const char *name, StructField *fields, int32_t field_count,
                         const Type **embedded, int32_t embed_count);

/** Return the struct name.  Asserts kind == TYPE_STRUCT. */
const char *type_struct_name(const Type *type);
/** Return the fields of a struct type.  Asserts kind == TYPE_STRUCT. */
const StructField *type_struct_fields(const Type *type);
/** Return the field count of a struct type.  Asserts kind == TYPE_STRUCT. */
int32_t type_struct_field_count(const Type *type);
/** Look up a field by name in a struct type.  Returns NULL if not found. */
const StructField *type_struct_find_field(const Type *type, const char *name);

/** Create a pointer type *pointee. */
Type *type_create_pointer(Arena *arena, const Type *pointee, bool is_mut);
/** Return the pointee type of a pointer.  Asserts kind == TYPE_POINTER. */
const Type *type_pointer_pointee(const Type *type);
/** Return whether a pointer type is mutable.  Asserts kind == TYPE_POINTER. */
bool type_pointer_is_mut(const Type *type);

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

#endif // RG_TYPES_H
