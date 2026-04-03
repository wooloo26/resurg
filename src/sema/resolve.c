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
            rsg_error(ast_type->location, "array element type required");
            analyzer->error_count++;
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
    rsg_error(ast_type->location, "unknown type '%s'", ast_type->name);
    analyzer->error_count++;
    return &TYPE_ERROR_INSTANCE;
}

// ── Literal ↔ type mapping ─────────────────────────────────────────────

const Type *literal_kind_to_type(LiteralKind kind) {
    switch (kind) {
    case LITERAL_BOOL:
        return &TYPE_BOOL_INSTANCE;
    case LITERAL_I8:
        return &TYPE_I8_INSTANCE;
    case LITERAL_I16:
        return &TYPE_I16_INSTANCE;
    case LITERAL_I32:
        return &TYPE_I32_INSTANCE;
    case LITERAL_I64:
        return &TYPE_I64_INSTANCE;
    case LITERAL_I128:
        return &TYPE_I128_INSTANCE;
    case LITERAL_U8:
        return &TYPE_U8_INSTANCE;
    case LITERAL_U16:
        return &TYPE_U16_INSTANCE;
    case LITERAL_U32:
        return &TYPE_U32_INSTANCE;
    case LITERAL_U64:
        return &TYPE_U64_INSTANCE;
    case LITERAL_U128:
        return &TYPE_U128_INSTANCE;
    case LITERAL_ISIZE:
        return &TYPE_ISIZE_INSTANCE;
    case LITERAL_USIZE:
        return &TYPE_USIZE_INSTANCE;
    case LITERAL_F32:
        return &TYPE_F32_INSTANCE;
    case LITERAL_F64:
        return &TYPE_F64_INSTANCE;
    case LITERAL_CHAR:
        return &TYPE_CHAR_INSTANCE;
    case LITERAL_STRING:
        return &TYPE_STRING_INSTANCE;
    case LITERAL_UNIT:
        return &TYPE_UNIT_INSTANCE;
    }
    return &TYPE_ERROR_INSTANCE;
}

LiteralKind type_to_literal_kind(TypeKind kind) {
    switch (kind) {
    case TYPE_BOOL:
        return LITERAL_BOOL;
    case TYPE_I8:
        return LITERAL_I8;
    case TYPE_I16:
        return LITERAL_I16;
    case TYPE_I32:
        return LITERAL_I32;
    case TYPE_I64:
        return LITERAL_I64;
    case TYPE_I128:
        return LITERAL_I128;
    case TYPE_U8:
        return LITERAL_U8;
    case TYPE_U16:
        return LITERAL_U16;
    case TYPE_U32:
        return LITERAL_U32;
    case TYPE_U64:
        return LITERAL_U64;
    case TYPE_U128:
        return LITERAL_U128;
    case TYPE_ISIZE:
        return LITERAL_ISIZE;
    case TYPE_USIZE:
        return LITERAL_USIZE;
    case TYPE_F32:
        return LITERAL_F32;
    case TYPE_F64:
        return LITERAL_F64;
    case TYPE_CHAR:
        return LITERAL_CHAR;
    case TYPE_STRING:
        return LITERAL_STRING;
    case TYPE_UNIT:
        return LITERAL_UNIT;
    default:
        return LITERAL_I32;
    }
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
