#include "_sema.h"

// ── Lookup helpers ─────────────────────────────────────────────────────

const Type *sema_lookup_type_alias(const Sema *analyzer, const char *name) {
    return hash_table_lookup(&analyzer->type_alias_table, name);
}

FnSignature *sema_lookup_fn(const Sema *analyzer, const char *name) {
    return hash_table_lookup(&analyzer->fn_table, name);
}

StructDef *sema_lookup_struct(const Sema *analyzer, const char *name) {
    return hash_table_lookup(&analyzer->struct_table, name);
}

EnumDef *sema_lookup_enum(const Sema *analyzer, const char *name) {
    return hash_table_lookup(&analyzer->enum_table, name);
}

PactDef *sema_lookup_pact(const Sema *analyzer, const char *name) {
    return hash_table_lookup(&analyzer->pact_table, name);
}

// ── AST type resolution ────────────────────────────────────────────────

const Type *resolve_ast_type(Sema *analyzer, const ASTType *ast_type) {
    if (ast_type == NULL || ast_type->kind == AST_TYPE_INFERRED) {
        return NULL;
    }
    if (ast_type->kind == AST_TYPE_ARRAY) {
        const Type *elem = resolve_ast_type(analyzer, ast_type->array_elem);
        if (elem == NULL) {
            SEMA_ERR(analyzer, ast_type->loc, "array elem type required");
            return &TYPE_ERR_INST;
        }
        return type_create_array(analyzer->arena, elem, ast_type->array_size);
    }
    if (ast_type->kind == AST_TYPE_SLICE) {
        const Type *elem = resolve_ast_type(analyzer, ast_type->slice_elem);
        if (elem == NULL) {
            SEMA_ERR(analyzer, ast_type->loc, "slice elem type required");
            return &TYPE_ERR_INST;
        }
        return type_create_slice(analyzer->arena, elem);
    }
    if (ast_type->kind == AST_TYPE_TUPLE) {
        const Type **elems = NULL;
        for (int32_t i = 0; i < BUF_LEN(ast_type->tuple_elems); i++) {
            const Type *elem = resolve_ast_type(analyzer, ast_type->tuple_elems[i]);
            if (elem == NULL) {
                elem = &TYPE_ERR_INST;
            }
            BUF_PUSH(elems, elem);
        }
        return type_create_tuple(analyzer->arena, elems, BUF_LEN(elems));
    }
    if (ast_type->kind == AST_TYPE_PTR) {
        const Type *pointee = resolve_ast_type(analyzer, ast_type->ptr_elem);
        if (pointee == NULL) {
            SEMA_ERR(analyzer, ast_type->loc, "ptr elem type required");
            return &TYPE_ERR_INST;
        }
        return type_create_ptr(analyzer->arena, pointee, false);
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
    SEMA_ERR(analyzer, ast_type->loc, "unknown type '%s'", ast_type->name);
    return &TYPE_ERR_INST;
}

// ── Lit ↔ type mapping ─────────────────────────────────────────────

/**
 * LitKind and TypeKind share the same ordering for the first 18
 * primitive entries.  Validate at compile time and use direct idxing.
 */
static_assert((int)LIT_BOOL == (int)TYPE_BOOL, "LitKind/TypeKind mismatch: BOOL");
static_assert((int)LIT_I32 == (int)TYPE_I32, "LitKind/TypeKind mismatch: I32");
static_assert((int)LIT_F64 == (int)TYPE_F64, "LitKind/TypeKind mismatch: F64");
static_assert((int)LIT_STR == (int)TYPE_STR, "LitKind/TypeKind mismatch: STR");
static_assert((int)LIT_UNIT == (int)TYPE_UNIT, "LitKind/TypeKind mismatch: UNIT");

const Type *lit_kind_to_type(LitKind kind) {
    return type_singleton((TypeKind)kind);
}

LitKind type_to_lit_kind(TypeKind kind) {
    // Safe cast: validated by static_asserts above.
    return (LitKind)kind;
}

// ── Lit promotion ──────────────────────────────────────────────────

const Type *promote_lit(ASTNode *lit, const Type *target) {
    if (lit == NULL || target == NULL) {
        return NULL;
    }

    // Handle negated lit: -(lit)
    if (lit->kind == NODE_UNARY && lit->unary.op == TOKEN_MINUS &&
        lit->unary.operand->kind == NODE_LIT) {
        const Type *result = promote_lit(lit->unary.operand, target);
        if (result != NULL) {
            lit->type = result;
            return result;
        }
        return NULL;
    }

    if (lit->kind != NODE_LIT) {
        return NULL;
    }

    // Promote integer lit (i32 default) to any integer type
    if (lit->lit.kind == LIT_I32 && type_is_integer(target)) {
        lit->lit.kind = type_to_lit_kind(target->kind);
        lit->type = target;
        return target;
    }

    // Promote integer lit to float type
    if (lit->lit.kind == LIT_I32 && type_is_float(target)) {
        lit->lit.kind = (target->kind == TYPE_F32) ? LIT_F32 : LIT_F64;
        lit->lit.float64_value = (double)lit->lit.integer_value;
        lit->type = target;
        return target;
    }

    // Promote u32 lit to larger unsigned types
    if (lit->lit.kind == LIT_U32 && type_is_unsigned_integer(target)) {
        lit->lit.kind = type_to_lit_kind(target->kind);
        lit->type = target;
        return target;
    }

    // Promote f64 lit to f32
    if (lit->lit.kind == LIT_F64 && target->kind == TYPE_F32) {
        lit->lit.kind = LIT_F32;
        lit->type = target;
        return target;
    }

    return NULL;
}
