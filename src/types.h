#ifndef RG_TYPES_H
#define RG_TYPES_H

#include "common.h"

/**
 * @file types.h
 * @brief Resolved types produced by semantic analysis.  v0.1.0 only needs
 * primitive types; each is represented as a singleton Type instance.
 */
typedef enum {
    TYPE_BOOL,
    TYPE_I32,
    TYPE_U32,
    TYPE_F64,
    TYPE_STRING,
    TYPE_UNIT,
    TYPE_ERROR, // sentinel for continued checking after type errors
} TypeKind;

typedef struct {
    TypeKind kind;
} Type;

/** Singleton type instances (avoids heap allocation for primitives). */
extern const Type TYPE_BOOL_INSTANCE;
extern const Type TYPE_I32_INSTANCE;
extern const Type TYPE_U32_INSTANCE;
extern const Type TYPE_F64_INSTANCE;
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
/** Return true if both types are non-NULL and have the same kind. */
bool type_equal(const Type *a, const Type *b);
/** Return true if @p type is i32, u32, or f64. */
bool type_is_numeric(const Type *type);
/** Return true if @p type is i32 or u32. */
bool type_is_integer(const Type *type);

#endif // RG_TYPES_H
