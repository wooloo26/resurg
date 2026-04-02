#ifndef RG_TYPES_H
#define RG_TYPES_H

#include "common.h"

// ---------------------------------------------------------------------------
// Resolved types — produced by semantic analysis.
// v0.1.0 only needs primitive types.
// ---------------------------------------------------------------------------
typedef enum {
    TYPE_BOOL,
    TYPE_I32,
    TYPE_U32,
    TYPE_F64,
    TYPE_STR,
    TYPE_UNIT,
    TYPE_ERROR, // sentinel for type errors (allows continued checking)
} TypeKind;

typedef struct {
    TypeKind kind;
} Type;

// Singleton type instances (avoids allocation for primitives)
extern Type g_type_bool_inst;
extern Type g_type_i32_inst;
extern Type g_type_u32_inst;
extern Type g_type_f64_inst;
extern Type g_type_str_inst;
extern Type g_type_unit_inst;
extern Type g_type_error_inst;

// Resolve a type name ("i32", "bool", etc.) to its singleton.
Type *type_from_name(const char *name);
// Return the Resurg name for a resolved type.
const char *type_name(const Type *t);
// Return the C type string for code generation.
const char *c_type_str(const Type *t);
// Return true if two types are equal.
bool type_eq(const Type *a, const Type *b);
// Return true if the type is numeric (i32, u32, or f64).
bool type_is_numeric(const Type *t);
// Return true if the type is an integer (i32 or u32).
bool type_is_integer(const Type *t);

#endif // RG_TYPES_H
