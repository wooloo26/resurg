#include "sema/_sema.h"

// ── Lookup helpers ─────────────────────────────────────────────────────

const Type *find_type_alias(const char *name) {
    for (int32_t i = 0; i < BUFFER_LENGTH(g_type_aliases); i++) {
        if (strcmp(g_type_aliases[i].name, name) == 0) {
            return g_type_aliases[i].underlying;
        }
    }
    return NULL;
}

FunctionSignature *find_function_signature(const char *name) {
    for (int32_t i = 0; i < BUFFER_LENGTH(g_function_signatures); i++) {
        if (strcmp(g_function_signatures[i].name, name) == 0) {
            return &g_function_signatures[i];
        }
    }
    return NULL;
}

// ── AST type resolution ────────────────────────────────────────────────

const Type *resolve_ast_type(SemanticAnalyzer *analyzer, const ASTType *ast_type) {
    if (ast_type == NULL || ast_type->kind == AST_TYPE_INFERRED) {
        return NULL;
    }
    if (ast_type->kind == AST_TYPE_ARRAY) {
        const Type *element = resolve_ast_type(analyzer, ast_type->array_element);
        if (element == NULL) {
            SEMA_ERROR(analyzer, ast_type->location, "array element type required");
            return &TYPE_ERROR_INSTANCE;
        }
        return type_create_array(analyzer->arena, element, ast_type->array_size);
    }
    if (ast_type->kind == AST_TYPE_TUPLE) {
        const Type **elements = NULL;
        for (int32_t i = 0; i < BUFFER_LENGTH(ast_type->tuple_elements); i++) {
            const Type *element = resolve_ast_type(analyzer, ast_type->tuple_elements[i]);
            if (element == NULL) {
                element = &TYPE_ERROR_INSTANCE;
            }
            BUFFER_PUSH(elements, element);
        }
        return type_create_tuple(analyzer->arena, elements, BUFFER_LENGTH(elements));
    }
    // AST_TYPE_NAME
    const Type *type = type_from_name(ast_type->name);
    if (type != NULL) {
        return type;
    }
    // Check type aliases
    const Type *alias = find_type_alias(ast_type->name);
    if (alias != NULL) {
        return alias;
    }
    SEMA_ERROR(analyzer, ast_type->location, "unknown type '%s'", ast_type->name);
    return &TYPE_ERROR_INSTANCE;
}

// ── Literal ↔ type mapping ─────────────────────────────────────────────

/**
 * LiteralKind and TypeKind share the same ordering for the first 18
 * primitive entries.  Validate at compile time and use direct indexing.
 */
static_assert((int)LITERAL_BOOL == (int)TYPE_BOOL, "LiteralKind/TypeKind mismatch: BOOL");
static_assert((int)LITERAL_I32 == (int)TYPE_I32, "LiteralKind/TypeKind mismatch: I32");
static_assert((int)LITERAL_F64 == (int)TYPE_F64, "LiteralKind/TypeKind mismatch: F64");
static_assert((int)LITERAL_STRING == (int)TYPE_STRING, "LiteralKind/TypeKind mismatch: STRING");
static_assert((int)LITERAL_UNIT == (int)TYPE_UNIT, "LiteralKind/TypeKind mismatch: UNIT");

/** Singleton instance table indexed by TypeKind (first 18 primitives). */
static const Type *const TYPE_INSTANCES[] = {
    &TYPE_BOOL_INSTANCE, &TYPE_I8_INSTANCE,     &TYPE_I16_INSTANCE,   &TYPE_I32_INSTANCE, &TYPE_I64_INSTANCE,
    &TYPE_I128_INSTANCE, &TYPE_U8_INSTANCE,     &TYPE_U16_INSTANCE,   &TYPE_U32_INSTANCE, &TYPE_U64_INSTANCE,
    &TYPE_U128_INSTANCE, &TYPE_ISIZE_INSTANCE,  &TYPE_USIZE_INSTANCE, &TYPE_F32_INSTANCE, &TYPE_F64_INSTANCE,
    &TYPE_CHAR_INSTANCE, &TYPE_STRING_INSTANCE, &TYPE_UNIT_INSTANCE,
};

static const int32_t TYPE_INSTANCE_COUNT = (int32_t)(sizeof(TYPE_INSTANCES) / sizeof(TYPE_INSTANCES[0]));

const Type *literal_kind_to_type(LiteralKind kind) {
    if ((int32_t)kind >= 0 && (int32_t)kind < TYPE_INSTANCE_COUNT) {
        return TYPE_INSTANCES[kind];
    }
    return &TYPE_ERROR_INSTANCE;
}

LiteralKind type_to_literal_kind(TypeKind kind) {
    if ((int32_t)kind >= 0 && (int32_t)kind < TYPE_INSTANCE_COUNT) {
        return (LiteralKind)kind;
    }
    return LITERAL_I32;
}

// ── Literal promotion ──────────────────────────────────────────────────

const Type *promote_literal(ASTNode *literal, const Type *target) {
    if (literal == NULL || target == NULL) {
        return NULL;
    }

    // Handle negated literal: -(literal)
    if (literal->kind == NODE_UNARY && literal->unary.op == TOKEN_MINUS &&
        literal->unary.operand->kind == NODE_LITERAL) {
        const Type *result = promote_literal(literal->unary.operand, target);
        if (result != NULL) {
            literal->type = result;
            return result;
        }
        return NULL;
    }

    if (literal->kind != NODE_LITERAL) {
        return NULL;
    }

    // Promote integer literal (i32 default) to any integer type
    if (literal->literal.kind == LITERAL_I32 && type_is_integer(target)) {
        literal->literal.kind = type_to_literal_kind(target->kind);
        literal->type = target;
        return target;
    }

    // Promote integer literal to float type
    if (literal->literal.kind == LITERAL_I32 && type_is_float(target)) {
        literal->literal.kind = (target->kind == TYPE_F32) ? LITERAL_F32 : LITERAL_F64;
        literal->literal.float64_value = (double)literal->literal.integer_value;
        literal->type = target;
        return target;
    }

    // Promote u32 literal to larger unsigned types
    if (literal->literal.kind == LITERAL_U32 && type_is_unsigned_integer(target)) {
        literal->literal.kind = type_to_literal_kind(target->kind);
        literal->type = target;
        return target;
    }

    // Promote f64 literal to f32
    if (literal->literal.kind == LITERAL_F64 && target->kind == TYPE_F32) {
        literal->literal.kind = LITERAL_F32;
        literal->type = target;
        return target;
    }

    return NULL;
}
