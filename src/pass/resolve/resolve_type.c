#include "_sema.h"

// ── Lookup helpers ─────────────────────────────────────────────────────

const Type *sema_lookup_type_alias(const Sema *sema, const char *name) {
    return hash_table_lookup(&sema->type_alias_table, name);
}

FnSig *sema_lookup_fn(const Sema *sema, const char *name) {
    return hash_table_lookup(&sema->fn_table, name);
}

StructDef *sema_lookup_struct(const Sema *sema, const char *name) {
    return hash_table_lookup(&sema->struct_table, name);
}

EnumDef *sema_lookup_enum(const Sema *sema, const char *name) {
    return hash_table_lookup(&sema->enum_table, name);
}

PactDef *sema_lookup_pact(const Sema *sema, const char *name) {
    return hash_table_lookup(&sema->pact_table, name);
}

GenericFnDef *sema_lookup_generic_fn(const Sema *sema, const char *name) {
    return hash_table_lookup(&sema->generic_fn_table, name);
}

GenericStructDef *sema_lookup_generic_struct(const Sema *sema, const char *name) {
    return hash_table_lookup(&sema->generic_struct_table, name);
}

GenericEnumDef *sema_lookup_generic_enum(const Sema *sema, const char *name) {
    return hash_table_lookup(&sema->generic_enum_table, name);
}

GenericTypeAlias *sema_lookup_generic_type_alias(const Sema *sema, const char *name) {
    return hash_table_lookup(&sema->generic_type_alias_table, name);
}

// ── Per-kind type resolvers ─────────────────────────────────────────────

static const Type *resolve_array_type(Sema *sema, const ASTType *ast_type) {
    const Type *elem = resolve_ast_type(sema, ast_type->array_elem);
    if (elem == NULL) {
        SEMA_ERR(sema, ast_type->loc, "array elem type required");
        return &TYPE_ERR_INST;
    }
    return type_create_array(sema->arena, elem, ast_type->array_size);
}

static const Type *resolve_slice_type(Sema *sema, const ASTType *ast_type) {
    const Type *elem = resolve_ast_type(sema, ast_type->slice_elem);
    if (elem == NULL) {
        SEMA_ERR(sema, ast_type->loc, "slice elem type required");
        return &TYPE_ERR_INST;
    }
    return type_create_slice(sema->arena, elem);
}

static const Type *resolve_tuple_type(Sema *sema, const ASTType *ast_type) {
    const Type **elems = NULL;
    for (int32_t i = 0; i < BUF_LEN(ast_type->tuple_elems); i++) {
        const Type *elem = resolve_ast_type(sema, ast_type->tuple_elems[i]);
        if (elem == NULL) {
            elem = &TYPE_ERR_INST;
        }
        BUF_PUSH(elems, elem);
    }
    return type_create_tuple(sema->arena, elems, BUF_LEN(elems));
}

static const Type *resolve_ptr_type(Sema *sema, const ASTType *ast_type) {
    const Type *pointee = resolve_ast_type(sema, ast_type->ptr_elem);
    if (pointee == NULL) {
        SEMA_ERR(sema, ast_type->loc, "ptr elem type required");
        return &TYPE_ERR_INST;
    }
    return type_create_ptr(sema->arena, pointee, false);
}

static const Type *resolve_option_type(Sema *sema, const ASTType *ast_type) {
    GenericEnumDef *gdef = sema_lookup_generic_enum(sema, "Option");
    if (gdef == NULL) {
        SEMA_ERR(sema, ast_type->loc, "built-in Option enum not found");
        return &TYPE_ERR_INST;
    }
    ASTType val_args[1] = {*ast_type->option_elem};
    GenericInstArgs inst_args = {val_args, 1, ast_type->loc};
    const char *mangled = instantiate_generic_enum(sema, gdef, &inst_args);
    if (mangled != NULL) {
        return sema_lookup_type_alias(sema, mangled);
    }
    return &TYPE_ERR_INST;
}

static const Type *resolve_result_type(Sema *sema, const ASTType *ast_type) {
    GenericEnumDef *gdef = sema_lookup_generic_enum(sema, "Result");
    if (gdef == NULL) {
        SEMA_ERR(sema, ast_type->loc, "built-in Result enum not found");
        return &TYPE_ERR_INST;
    }
    ASTType val_args[2] = {*ast_type->result_ok, *ast_type->result_err};
    GenericInstArgs inst_args = {val_args, 2, ast_type->loc};
    const char *mangled = instantiate_generic_enum(sema, gdef, &inst_args);
    if (mangled != NULL) {
        return sema_lookup_type_alias(sema, mangled);
    }
    return &TYPE_ERR_INST;
}

static const Type *resolve_fn_type(Sema *sema, const ASTType *ast_type) {
    const Type **params = NULL;
    for (int32_t i = 0; i < BUF_LEN(ast_type->fn_param_types); i++) {
        const Type *pt = resolve_ast_type(sema, ast_type->fn_param_types[i]);
        if (pt == NULL) {
            pt = &TYPE_ERR_INST;
        }
        BUF_PUSH(params, pt);
    }
    const Type *ret = NULL;
    if (ast_type->fn_return_type != NULL) {
        ret = resolve_ast_type(sema, ast_type->fn_return_type);
    }
    if (ret == NULL) {
        ret = &TYPE_UNIT_INST;
    }
    FnTypeSpec fn_spec = {params, BUF_LEN(params), ret, ast_type->fn_kind};
    return type_create_fn(sema->arena, &fn_spec);
}

static const Type *resolve_name_type(Sema *sema, const ASTType *ast_type) {
    const Type *type = type_from_name(ast_type->name);
    if (type != NULL) {
        return type;
    }
    // Check active type param substitutions (for generic body checking)
    const Type *tp = hash_table_lookup(&sema->type_param_table, ast_type->name);
    if (tp != NULL) {
        return tp;
    }
    // Check type aliases
    const Type *alias = sema_lookup_type_alias(sema, ast_type->name);
    if (alias == NULL && sema->current_scope != NULL && sema->current_scope->module_name != NULL) {
        const char *qualified =
            arena_sprintf(sema->arena, "%s.%s", sema->current_scope->module_name, ast_type->name);
        alias = sema_lookup_type_alias(sema, qualified);
    }
    if (alias != NULL) {
        return alias;
    }
    // Check generic type aliases with type args
    if (BUF_LEN(ast_type->type_args) > 0) {
        GenericTypeAlias *gta = sema_lookup_generic_type_alias(sema, ast_type->name);
        if (gta != NULL) {
            int32_t expected = gta->type_param_count;
            int32_t got = BUF_LEN(ast_type->type_args);
            if (got != expected) {
                SEMA_ERR(sema, ast_type->loc,
                         "wrong number of type arguments for '%s': expected %d, got %d",
                         ast_type->name, expected, got);
                return &TYPE_ERR_INST;
            }
            // Push type param substitutions
            for (int32_t i = 0; i < expected; i++) {
                const Type *t = resolve_ast_type(sema, ast_type->type_args[i]);
                if (t == NULL) {
                    t = &TYPE_ERR_INST;
                }
                hash_table_insert(&sema->type_param_table, gta->type_params[i].name, (void *)t);
            }
            const Type *result = resolve_ast_type(sema, &gta->alias_type);
            // Clear type param substitutions
            for (int32_t i = 0; i < expected; i++) {
                hash_table_remove(&sema->type_param_table, gta->type_params[i].name);
            }
            return result;
        }
        // Check generic structs with type args (e.g., Pair<i32, str>)
        GenericStructDef *gsdef = sema_lookup_generic_struct(sema, ast_type->name);
        if (gsdef != NULL) {
            int32_t got = BUF_LEN(ast_type->type_args);
            // Convert ASTType** (ptr buf) to ASTType* (value buf)
            ASTType *val_args = NULL;
            for (int32_t i = 0; i < got; i++) {
                BUF_PUSH(val_args, *ast_type->type_args[i]);
            }
            GenericInstArgs inst_args = {val_args, got, ast_type->loc};
            const char *mangled = instantiate_generic_struct(sema, gsdef, &inst_args);
            BUF_FREE(val_args);
            if (mangled != NULL) {
                return sema_lookup_type_alias(sema, mangled);
            }
            return &TYPE_ERR_INST;
        }
        // Check generic enums with type args (e.g., Either<i32, str>)
        GenericEnumDef *gedef = sema_lookup_generic_enum(sema, ast_type->name);
        if (gedef != NULL) {
            int32_t got = BUF_LEN(ast_type->type_args);
            ASTType *val_args = NULL;
            for (int32_t i = 0; i < got; i++) {
                BUF_PUSH(val_args, *ast_type->type_args[i]);
            }
            GenericInstArgs inst_args = {val_args, got, ast_type->loc};
            const char *mangled = instantiate_generic_enum(sema, gedef, &inst_args);
            BUF_FREE(val_args);
            if (mangled != NULL) {
                return sema_lookup_type_alias(sema, mangled);
            }
            return &TYPE_ERR_INST;
        }
    }
    SEMA_ERR(sema, ast_type->loc, "unknown type '%s'", ast_type->name);
    return &TYPE_ERR_INST;
}

// ── Dispatch table ─────────────────────────────────────────────────────

typedef const Type *(*TypeResolver)(Sema *, const ASTType *);

static const TypeResolver TYPE_RESOLVERS[] = {
    [AST_TYPE_NAME] = resolve_name_type,     [AST_TYPE_ARRAY] = resolve_array_type,
    [AST_TYPE_SLICE] = resolve_slice_type,   [AST_TYPE_TUPLE] = resolve_tuple_type,
    [AST_TYPE_PTR] = resolve_ptr_type,       [AST_TYPE_OPTION] = resolve_option_type,
    [AST_TYPE_RESULT] = resolve_result_type, [AST_TYPE_FN] = resolve_fn_type,
};

#define TYPE_RESOLVER_COUNT ((int32_t)(sizeof(TYPE_RESOLVERS) / sizeof(TYPE_RESOLVERS[0])))

// ── AST type resolution ────────────────────────────────────────────────

const Type *resolve_ast_type(Sema *sema, const ASTType *ast_type) {
    if (ast_type == NULL || ast_type->kind == AST_TYPE_INFERRED) {
        return NULL;
    }
    int32_t kind = (int32_t)ast_type->kind;
    if (kind >= 0 && kind < TYPE_RESOLVER_COUNT) {
        TypeResolver resolver = TYPE_RESOLVERS[kind];
        if (resolver != NULL) {
            return resolver(sema, ast_type);
        }
    }
    return NULL;
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
