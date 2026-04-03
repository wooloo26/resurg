#ifndef RG_TYPES_H
#define RG_TYPES_H

#include "common.h"

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
    TYPE_ARRAY, // [N]T
    TYPE_TUPLE, // (A, B, ...)
    TYPE_ERROR, // sentinel for continued checking after type errors
} TypeKind;

typedef struct Type Type;

struct Type {
    TypeKind kind;
    // TYPE_ARRAY fields
    const Type *array_element;
    int32_t array_size;
    // TYPE_TUPLE fields
    const Type **tuple_elements; /* buf */
    int32_t tuple_count;
};

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
const char *type_name(const Type *type);
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

/** Create an array type [size]element. */
Type *type_create_array(Arena *arena, const Type *element, int32_t size);
/** Create a tuple type from an array of element types. */
Type *type_create_tuple(Arena *arena, const Type **elements, int32_t count);

#endif // RG_TYPES_H
