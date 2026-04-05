#include "_sema.h"

// ── Lookup helpers ─────────────────────────────────────────────────────

const Type *sema_lookup_type_alias(const SemanticAnalyzer *analyzer, const char *name) {
    return hash_table_lookup(&analyzer->type_alias_table, name);
}

FunctionSignature *sema_lookup_function(const SemanticAnalyzer *analyzer, const char *name) {
    return hash_table_lookup(&analyzer->function_table, name);
}

StructDefinition *sema_lookup_struct(const SemanticAnalyzer *analyzer, const char *name) {
    return hash_table_lookup(&analyzer->struct_table, name);
}

EnumDefinition *sema_lookup_enum(const SemanticAnalyzer *analyzer, const char *name) {
    return hash_table_lookup(&analyzer->enum_table, name);
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
    if (ast_type->kind == AST_TYPE_POINTER) {
        const Type *pointee = resolve_ast_type(analyzer, ast_type->pointer_element);
        if (pointee == NULL) {
            SEMA_ERROR(analyzer, ast_type->location, "pointer element type required");
            return &TYPE_ERROR_INSTANCE;
        }
        return type_create_pointer(analyzer->arena, pointee, false);
    }
    // AST_TYPE_NAME
    const Type *type = type_from_name(ast_type->name);
    if (type != NULL) {
        return type;
    }
    // Check type aliases
    const Type *alias = sema_lookup_type_alias(analyzer, ast_type->name);
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

const Type *literal_kind_to_type(LiteralKind kind) {
    return type_singleton((TypeKind)kind);
}

LiteralKind type_to_literal_kind(TypeKind kind) {
    // Safe cast: validated by static_asserts above.
    return (LiteralKind)kind;
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
