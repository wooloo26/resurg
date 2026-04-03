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
 * Bidirectional mapping between LiteralKind and TypeKind.
 * Both enums share the same ordering for the first 18 entries.
 */
static const struct {
    LiteralKind literal;
    TypeKind type;
    const Type *instance;
} LITERAL_TYPE_MAP[] = {
    {LITERAL_BOOL, TYPE_BOOL, &TYPE_BOOL_INSTANCE},
    {LITERAL_I8, TYPE_I8, &TYPE_I8_INSTANCE},
    {LITERAL_I16, TYPE_I16, &TYPE_I16_INSTANCE},
    {LITERAL_I32, TYPE_I32, &TYPE_I32_INSTANCE},
    {LITERAL_I64, TYPE_I64, &TYPE_I64_INSTANCE},
    {LITERAL_I128, TYPE_I128, &TYPE_I128_INSTANCE},
    {LITERAL_U8, TYPE_U8, &TYPE_U8_INSTANCE},
    {LITERAL_U16, TYPE_U16, &TYPE_U16_INSTANCE},
    {LITERAL_U32, TYPE_U32, &TYPE_U32_INSTANCE},
    {LITERAL_U64, TYPE_U64, &TYPE_U64_INSTANCE},
    {LITERAL_U128, TYPE_U128, &TYPE_U128_INSTANCE},
    {LITERAL_ISIZE, TYPE_ISIZE, &TYPE_ISIZE_INSTANCE},
    {LITERAL_USIZE, TYPE_USIZE, &TYPE_USIZE_INSTANCE},
    {LITERAL_F32, TYPE_F32, &TYPE_F32_INSTANCE},
    {LITERAL_F64, TYPE_F64, &TYPE_F64_INSTANCE},
    {LITERAL_CHAR, TYPE_CHAR, &TYPE_CHAR_INSTANCE},
    {LITERAL_STRING, TYPE_STRING, &TYPE_STRING_INSTANCE},
    {LITERAL_UNIT, TYPE_UNIT, &TYPE_UNIT_INSTANCE},
};

static const int32_t LITERAL_TYPE_MAP_COUNT = (int32_t)(sizeof(LITERAL_TYPE_MAP) / sizeof(LITERAL_TYPE_MAP[0]));

const Type *literal_kind_to_type(LiteralKind kind) {
    for (int32_t i = 0; i < LITERAL_TYPE_MAP_COUNT; i++) {
        if (LITERAL_TYPE_MAP[i].literal == kind) {
            return LITERAL_TYPE_MAP[i].instance;
        }
    }
    return &TYPE_ERROR_INSTANCE;
}

LiteralKind type_to_literal_kind(TypeKind kind) {
    for (int32_t i = 0; i < LITERAL_TYPE_MAP_COUNT; i++) {
        if (LITERAL_TYPE_MAP[i].type == kind) {
            return LITERAL_TYPE_MAP[i].literal;
        }
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
