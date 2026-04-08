#include "_lower.h"

// ── Expression lower ───────────────────────────────────────────────

static HirNode *lower_lit(Lower *low, const ASTNode *ast) {
    SrcLoc loc = ast->loc;
    const Type *type = ast->type;

    switch (ast->lit.kind) {
    case LIT_BOOL: {
        HirNode *node = hir_new(low->hir_arena, HIR_BOOL_LIT, type, loc);
        node->bool_lit.value = ast->lit.boolean_value;
        return node;
    }
    case LIT_I8:
    case LIT_I16:
    case LIT_I32:
    case LIT_I64:
    case LIT_I128:
    case LIT_U8:
    case LIT_U16:
    case LIT_U32:
    case LIT_U64:
    case LIT_U128:
    case LIT_ISIZE:
    case LIT_USIZE: {
        HirNode *node = hir_new(low->hir_arena, HIR_INT_LIT, type, loc);
        node->int_lit.value = ast->lit.integer_value;
        node->int_lit.int_kind = (TypeKind)ast->lit.kind;
        return node;
    }
    case LIT_F32:
    case LIT_F64: {
        HirNode *node = hir_new(low->hir_arena, HIR_FLOAT_LIT, type, loc);
        node->float_lit.value = ast->lit.float64_value;
        node->float_lit.float_kind = (TypeKind)ast->lit.kind;
        return node;
    }
    case LIT_CHAR: {
        HirNode *node = hir_new(low->hir_arena, HIR_CHAR_LIT, type, loc);
        node->char_lit.value = ast->lit.char_value;
        return node;
    }
    case LIT_STR: {
        HirNode *node = hir_new(low->hir_arena, HIR_STR_LIT, type, loc);
        node->str_lit.value = ast->lit.str_value;
        return node;
    }
    case LIT_UNIT:
        return hir_new(low->hir_arena, HIR_UNIT_LIT, type, loc);
    }
    return hir_new(low->hir_arena, HIR_UNIT_LIT, &TYPE_ERR_INST, loc);
}

static HirNode *lower_id(Lower *low, const ASTNode *ast) {
    const char *name = ast->id.name;
    HirSym *sym = lower_scope_lookup(low, name);
    if (sym == NULL) {
        // Create an unresolved sym with whatever type sema assigned.
        HirSymSpec id_spec = {HIR_SYM_VAR, name, ast->type, false, ast->loc};
        sym = lower_add_var(low, &id_spec);
    }
    HirNode *node = hir_new(low->hir_arena, HIR_VAR_REF,
                            ast->type != NULL ? ast->type : hir_sym_type(sym), ast->loc);
    node->var_ref.sym = sym;
    return node;
}

static HirNode *lower_unary(Lower *low, const ASTNode *ast) {
    HirNode *operand = lower_expr(low, ast->unary.operand);
    HirNode *node = hir_new(low->hir_arena, HIR_UNARY, ast->type, ast->loc);
    node->unary.op = ast->unary.op;
    node->unary.operand = operand;
    return node;
}

/** Negate a boolean expr with a unary `!`. */
static HirNode *build_bool_negate(Lower *low, HirNode *expr, SrcLoc loc) {
    HirNode *neg = hir_new(low->hir_arena, HIR_UNARY, &TYPE_BOOL_INST, loc);
    neg->unary.op = TOKEN_BANG;
    neg->unary.operand = expr;
    return neg;
}

/**
 * Build an equality check for a single elem pair.
 *
 * For TYPE_STR elems, emits rsg_str_equal(l, r) (negated for !=).
 * Otherwise emits a plain binary comparison.
 */
static HirNode *build_equality_check(Lower *low, HirNode *left_elem, HirNode *right_elem,
                                     TokenKind elem_op) {
    SrcLoc loc = left_elem->loc;
    const Type *elem_type = left_elem->type;
    if (elem_type->kind == TYPE_STR) {
        HirNode **args = NULL;
        BUF_PUSH(args, left_elem);
        BUF_PUSH(args, right_elem);
        HirNode *cmp = lower_make_builtin_call(
            low, &(BuiltinCallSpec){"rsg_str_equal", &TYPE_BOOL_INST, args, loc});
        if (elem_op != TOKEN_EQUAL_EQUAL) {
            cmp = build_bool_negate(low, cmp, loc);
        }
        return cmp;
    }
    HirNode *cmp = hir_new(low->hir_arena, HIR_BINARY, &TYPE_BOOL_INST, loc);
    cmp->binary.op = elem_op;
    cmp->binary.left = left_elem;
    cmp->binary.right = right_elem;
    return cmp;
}

/** Join @p cmp into the running @p result chain with @p join_op. */
static HirNode *join_comparison(Lower *low, HirNode *result, HirNode *cmp, TokenKind join_op,
                                SrcLoc loc) {
    if (result == NULL) {
        return cmp;
    }
    HirNode *join = hir_new(low->hir_arena, HIR_BINARY, &TYPE_BOOL_INST, loc);
    join->binary.op = join_op;
    join->binary.left = result;
    join->binary.right = cmp;
    return join;
}

/** Grouped params for building an elem access. */
typedef struct {
    HirSym *sym;
    int32_t idx;
    const Type *elem_type;
    HirNodeKind kind;
    SrcLoc loc;
} ElemAccessSpec;

/** Build an elem access: array idx or tuple idx depending on @p spec->kind. */
static HirNode *build_elem_access(Lower *low, const ElemAccessSpec *spec) {
    HirNode *ref = lower_make_var_ref(low, spec->sym, spec->loc);
    if (spec->kind == HIR_IDX) {
        IntLitSpec idx_spec = {(uint64_t)spec->idx, &TYPE_I32_INST, TYPE_I32, spec->loc};
        HirNode *idx = lower_make_int_lit(low, &idx_spec);
        HirNode *elem = hir_new(low->hir_arena, HIR_IDX, spec->elem_type, spec->loc);
        elem->idx_access.object = ref;
        elem->idx_access.idx = idx;
        return elem;
    }
    HirNode *elem = hir_new(low->hir_arena, HIR_TUPLE_IDX, spec->elem_type, spec->loc);
    elem->tuple_idx.object = ref;
    elem->tuple_idx.elem_idx = spec->idx;
    return elem;
}

/** Grouped params for a binary equality lowering. */
typedef struct {
    HirNode *left;
    HirNode *right;
    TokenKind op;
    SrcLoc loc;
} EqualitySpec;

/**
 * Expand compound (array/tuple) equality:
 * `a == b` → `{ var l=a; var r=b; l[0]==r[0] && l[1]==r[1] && ... }`
 */
static HirNode *lower_compound_equality(Lower *low, const EqualitySpec *eq) {
    const Type *type = eq->left->type;
    bool is_array = (type->kind == TYPE_ARRAY);
    int32_t count = is_array ? type->array.size : type->tuple.count;
    bool is_equal = (eq->op == TOKEN_EQUAL_EQUAL);
    TokenKind elem_op = is_equal ? TOKEN_EQUAL_EQUAL : TOKEN_BANG_EQUAL;
    TokenKind join_op = is_equal ? TOKEN_AMPERSAND_AMPERSAND : TOKEN_PIPE_PIPE;

    const char *left_name = lower_make_temp_name(low);
    HirSymSpec left_spec = {HIR_SYM_VAR, left_name, type, false, eq->loc};
    HirSym *left_sym = lower_add_var(low, &left_spec);

    const char *right_name = lower_make_temp_name(low);
    HirSymSpec right_spec = {HIR_SYM_VAR, right_name, type, false, eq->loc};
    HirSym *right_sym = lower_add_var(low, &right_spec);

    HirNode *result = NULL;
    HirNodeKind access_kind = is_array ? HIR_IDX : HIR_TUPLE_IDX;
    for (int32_t i = 0; i < count; i++) {
        const Type *elem_type = is_array ? type->array.elem : type->tuple.elems[i];
        ElemAccessSpec left_ea = {left_sym, i, elem_type, access_kind, eq->loc};
        ElemAccessSpec right_ea = {right_sym, i, elem_type, access_kind, eq->loc};
        HirNode *left_elem = build_elem_access(low, &left_ea);
        HirNode *right_elem = build_elem_access(low, &right_ea);
        HirNode *cmp = build_equality_check(low, left_elem, right_elem, elem_op);
        result = join_comparison(low, result, cmp, join_op, eq->loc);
    }

    if (result == NULL) {
        HirNode *lit = hir_new(low->hir_arena, HIR_BOOL_LIT, &TYPE_BOOL_INST, eq->loc);
        lit->bool_lit.value = is_equal;
        return lit;
    }

    HirNode **stmts = NULL;
    BUF_PUSH(stmts, lower_make_var_decl(low, left_sym, eq->left));
    BUF_PUSH(stmts, lower_make_var_decl(low, right_sym, eq->right));

    HirNode *block = hir_new(low->hir_arena, HIR_BLOCK, &TYPE_BOOL_INST, eq->loc);
    block->block.stmts = stmts;
    block->block.result = result;
    return block;
}

/** Lower str equality: rsg_str_equal(l, r), negated for !=. */
static HirNode *lower_str_equality(Lower *low, const EqualitySpec *eq) {
    HirNode **args = NULL;
    BUF_PUSH(args, eq->left);
    BUF_PUSH(args, eq->right);
    HirNode *call = lower_make_builtin_call(
        low, &(BuiltinCallSpec){"rsg_str_equal", &TYPE_BOOL_INST, args, eq->loc});
    if (eq->op == TOKEN_BANG_EQUAL) {
        return build_bool_negate(low, call, eq->loc);
    }
    return call;
}

static HirNode *lower_binary(Lower *low, const ASTNode *ast) {
    HirNode *left = lower_expr(low, ast->binary.left);
    HirNode *right = lower_expr(low, ast->binary.right);
    TokenKind op = ast->binary.op;
    const Type *left_type = left->type;

    // Str equality/inequality → rsg_str_equal call
    if (left_type != NULL && left_type->kind == TYPE_STR &&
        (op == TOKEN_EQUAL_EQUAL || op == TOKEN_BANG_EQUAL)) {
        return lower_str_equality(low, &(EqualitySpec){left, right, op, ast->loc});
    }

    // Unit equality/inequality → constant true/false
    if (left_type != NULL && left_type->kind == TYPE_UNIT &&
        (op == TOKEN_EQUAL_EQUAL || op == TOKEN_BANG_EQUAL)) {
        HirNode *lit = hir_new(low->hir_arena, HIR_BOOL_LIT, &TYPE_BOOL_INST, ast->loc);
        lit->bool_lit.value = (op == TOKEN_EQUAL_EQUAL);
        return lit;
    }

    // Array/tuple equality/inequality → elem-wise comparison
    if (left_type != NULL && (left_type->kind == TYPE_ARRAY || left_type->kind == TYPE_TUPLE) &&
        (op == TOKEN_EQUAL_EQUAL || op == TOKEN_BANG_EQUAL)) {
        return lower_compound_equality(low, &(EqualitySpec){left, right, op, ast->loc});
    }

    HirNode *node = hir_new(low->hir_arena, HIR_BINARY, ast->type, ast->loc);
    node->binary.op = op;
    node->binary.left = left;
    node->binary.right = right;
    return node;
}

/** Return true if the callee AST node resolves to the builtin "assert". */
static bool is_assert_callee(const ASTNode *callee) {
    if (callee->kind != NODE_ID) {
        return false;
    }
    return strcmp(callee->id.name, "assert") == 0;
}

/** Return true if the callee AST resolves to "print" or "println". */
static bool is_print_callee(const ASTNode *callee, bool *out_newline) {
    if (callee->kind != NODE_ID) {
        return false;
    }
    if (strcmp(callee->id.name, "println") == 0) {
        *out_newline = true;
        return true;
    }
    if (strcmp(callee->id.name, "print") == 0) {
        *out_newline = false;
        return true;
    }
    return false;
}

/** Resolve the rsg_print[ln]_* fn name for @p type, derived from type_name(). */
static const char *resolve_print_fn(Arena *arena, const Type *type, bool newline) {
    if (!type_is_printable(type)) {
        return NULL;
    }
    const char *name = type_name(arena, type);
    return arena_sprintf(arena, "rsg_print%s_%s", newline ? "ln" : "", name);
}

/**
 * Expand print/println(arg) → rsg_print[ln]_TYPE(arg).
 *
 * Dispatches to the type-specific runtime fn based on the arg type.
 */
static HirNode *lower_print_call(Lower *low, const ASTNode *ast, bool newline) {
    SrcLoc loc = ast->loc;
    int32_t arg_count = BUF_LEN(ast->call.args);
    if (arg_count == 0) {
        return hir_new(low->hir_arena, HIR_UNIT_LIT, &TYPE_UNIT_INST, loc);
    }

    HirNode *arg = lower_expr(low, ast->call.args[0]);
    const char *fn_name = resolve_print_fn(low->hir_arena, arg->type, newline);
    if (fn_name == NULL) {
        return hir_new(low->hir_arena, HIR_UNIT_LIT, &TYPE_UNIT_INST, loc);
    }

    HirNode **args = NULL;
    BUF_PUSH(args, arg);
    return lower_make_builtin_call(low, &(BuiltinCallSpec){fn_name, &TYPE_UNIT_INST, args, loc});
}

/**
 * Expand assert(cond, msg?) → rsg_assert(cond, msg_or_null, file, line).
 *
 * When the msg is a str lit, pass its raw value directly as a
 * HIR_STR_LIT so codegen can emit a plain C str.  For non-lit
 * str exprs, lower wraps them unchanged — codegen extracts
 * `.data` from the emitted rsg_str.
 */
static HirNode *lower_assert_call(Lower *low, const ASTNode *ast) {
    int32_t arg_count = BUF_LEN(ast->call.args);
    SrcLoc loc = ast->loc;

    // Condition (default to false if missing)
    HirNode *cond;
    if (arg_count > 0) {
        cond = lower_expr(low, ast->call.args[0]);
    } else {
        cond = hir_new(low->hir_arena, HIR_BOOL_LIT, &TYPE_BOOL_INST, loc);
        cond->bool_lit.value = false;
    }

    // Message (NULL when absent; str lit kept as lit, expr passed through)
    HirNode *msg;
    if (arg_count > 1) {
        msg = lower_expr(low, ast->call.args[1]);
    } else {
        // Pass NULL sentinel — codegen emits "NULL" for this
        msg = hir_new(low->hir_arena, HIR_UNIT_LIT, &TYPE_UNIT_INST, loc);
    }

    // File name as str lit
    HirNode *file_node = hir_new(low->hir_arena, HIR_STR_LIT, &TYPE_STR_INST, loc);
    file_node->str_lit.value = loc.file != NULL ? loc.file : "<unknown>";

    // Line number as i32 lit
    IntLitSpec line_spec = {(uint64_t)loc.line, &TYPE_I32_INST, TYPE_I32, loc};
    HirNode *line_node = lower_make_int_lit(low, &line_spec);

    HirNode **args = NULL;
    BUF_PUSH(args, cond);
    BUF_PUSH(args, msg);
    BUF_PUSH(args, file_node);
    BUF_PUSH(args, line_node);

    return lower_make_builtin_call(low,
                                   &(BuiltinCallSpec){"rsg_assert", &TYPE_UNIT_INST, args, loc});
}

/** Lower a buf of AST expr nodes into a buf of HIR nodes. */
static HirNode **lower_elem_list(Lower *low, ASTNode **ast_elems) {
    HirNode **elems = NULL;
    for (int32_t i = 0; i < BUF_LEN(ast_elems); i++) {
        BUF_PUSH(elems, lower_expr(low, ast_elems[i]));
    }
    return elems;
}

// Forward decls for enum init helpers
typedef struct {
    const Type *enum_type;
    const char *variant_name;
    SrcLoc loc;
} EnumVariantSpec;

typedef struct {
    const EnumVariant *variant;
    const char **field_names;
    HirNode **field_values;
} EnumInitState;

static HirNode *lower_enum_unit_init(Lower *low, const EnumVariantSpec *spec);
static HirNode *lower_enum_tuple_init(Lower *low, const EnumVariantSpec *spec, ASTNode **args);
static HirNode *lower_enum_struct_init(Lower *low, const EnumVariantSpec *spec,
                                       const char **ast_field_names, ASTNode **ast_field_values);

/**
 * Try to lower a member-callee call as a method call.
 *
 * Handles enum variant construction, enum method calls, and struct
 * method calls (including promoted methods from embedded structs).
 * Returns NULL when the callee is not a recognized method.
 */
/** Walk embedded structs to find a promoted method sym. */
static HirSym *find_promoted_method(Lower *low, const Type *struct_type, const char *method_name,
                                    HirNode **recv_ptr, bool via_ptr) {
    for (int32_t i = 0; i < struct_type->struct_type.embed_count; i++) {
        const Type *embed_type = struct_type->struct_type.embedded[i];
        const char *embed_key =
            arena_sprintf(low->hir_arena, "%s.%s", embed_type->struct_type.name, method_name);
        HirSym *method_sym = lower_scope_lookup(low, embed_key);
        if (method_sym != NULL) {
            *recv_ptr = lower_make_field_access(
                low, &(FieldAccessSpec){*recv_ptr, embed_type->struct_type.name, embed_type,
                                        via_ptr, (*recv_ptr)->loc});
            return method_sym;
        }
    }
    return NULL;
}

/** Build a HIR_METHOD_CALL node from a resolved method sym. */
static HirNode *build_method_call(Lower *low, const ASTNode *ast, HirNode *recv,
                                  const HirSym *method_sym) {
    HirNode **args = lower_elem_list(low, ast->call.args);
    HirNode *node = hir_new(low->hir_arena, HIR_METHOD_CALL, ast->type, ast->loc);
    node->method_call.recv = method_sym->is_static ? NULL : recv;
    node->method_call.mangled_name = method_sym->mangled_name;
    node->method_call.args = args;
    node->method_call.is_ptr_recv = method_sym->is_ptr_recv;
    return node;
}

static HirNode *lower_member_call(Lower *low, const ASTNode *ast) {
    const ASTNode *member_ast = ast->call.callee;
    const Type *obj_type = member_ast->member.object->type;

    bool via_ptr = false;
    if (obj_type != NULL && obj_type->kind == TYPE_PTR) {
        obj_type = obj_type->ptr.pointee;
        via_ptr = true;
    }

    // Enum: tuple variant construction, unit variant, or method call
    if (obj_type != NULL && obj_type->kind == TYPE_ENUM) {
        const char *variant_name = member_ast->member.member;
        const EnumVariant *variant = type_enum_find_variant(obj_type, variant_name);
        if (variant != NULL && variant->kind == ENUM_VARIANT_TUPLE) {
            EnumVariantSpec spec = {obj_type, variant_name, ast->loc};
            return lower_enum_tuple_init(low, &spec, ast->call.args);
        }
        if (variant != NULL && variant->kind == ENUM_VARIANT_UNIT) {
            EnumVariantSpec spec = {obj_type, variant_name, ast->loc};
            return lower_enum_unit_init(low, &spec);
        }
        const char *method_key =
            arena_sprintf(low->hir_arena, "%s.%s", type_enum_name(obj_type), variant_name);
        HirSym *method_sym = lower_scope_lookup(low, method_key);
        if (method_sym != NULL) {
            HirNode *recv = lower_expr(low, member_ast->member.object);
            return build_method_call(low, ast, recv, method_sym);
        }
    }

    // Struct: direct method or promoted method from embedded struct
    if (obj_type != NULL && obj_type->kind == TYPE_STRUCT) {
        const char *method_name = member_ast->member.member;
        HirNode *recv = lower_expr(low, member_ast->member.object);

        if (!via_ptr && low->current_recv != NULL && low->current_is_ptr_recv &&
            recv->kind == HIR_VAR_REF && recv->var_ref.sym == low->current_recv) {
            via_ptr = true;
        }

        const char *key =
            arena_sprintf(low->hir_arena, "%s.%s", obj_type->struct_type.name, method_name);
        HirSym *method_sym = lower_scope_lookup(low, key);

        if (method_sym == NULL) {
            method_sym = find_promoted_method(low, obj_type, method_name, &recv, via_ptr);
        }

        if (method_sym != NULL) {
            return build_method_call(low, ast, recv, method_sym);
        }
    }

    // Primitive ext methods: look up "type_name.method_name"
    if (obj_type != NULL) {
        const char *prim_name = type_name(low->hir_arena, obj_type);
        if (prim_name != NULL) {
            const char *method_name = member_ast->member.member;
            const char *key = arena_sprintf(low->hir_arena, "%s.%s", prim_name, method_name);
            HirSym *method_sym = lower_scope_lookup(low, key);
            if (method_sym != NULL) {
                HirNode *recv = lower_expr(low, member_ast->member.object);
                return build_method_call(low, ast, recv, method_sym);
            }
        }
    }

    return NULL;
}

static HirNode *lower_call(Lower *low, const ASTNode *ast) {
    if (is_assert_callee(ast->call.callee)) {
        return lower_assert_call(low, ast);
    }

    bool newline = false;
    if (is_print_callee(ast->call.callee, &newline)) {
        return lower_print_call(low, ast, newline);
    }

    if (ast->call.callee->kind == NODE_MEMBER) {
        HirNode *method = lower_member_call(low, ast);
        if (method != NULL) {
            return method;
        }
    }

    HirNode *callee = lower_expr(low, ast->call.callee);
    HirNode **args = lower_elem_list(low, ast->call.args);
    HirNode *node = hir_new(low->hir_arena, HIR_CALL, ast->type, ast->loc);
    node->call.callee = callee;
    node->call.args = args;
    return node;
}

static HirNode *lower_member(Lower *low, const ASTNode *ast) {
    HirNode *object = lower_expr(low, ast->member.object);

    // Tuple member access: tuple.0, tuple.1, ...
    if (object->type != NULL && object->type->kind == TYPE_TUPLE) {
        char *end = NULL;
        long idx = strtol(ast->member.member, &end, 10);
        if (end != NULL && *end == '\0') {
            HirNode *node = hir_new(low->hir_arena, HIR_TUPLE_IDX, ast->type, ast->loc);
            node->tuple_idx.object = object;
            node->tuple_idx.elem_idx = (int32_t)idx;
            return node;
        }
    }

    // Auto-deref for ptr types: p.field → p->field
    bool via_ptr = false;
    const Type *lookup_type = object->type;
    if (lookup_type != NULL && lookup_type->kind == TYPE_PTR) {
        lookup_type = lookup_type->ptr.pointee;
        via_ptr = true;
    }

    // Slice .len → struct field access on "len" (matches RsgSlice C field)
    if (lookup_type != NULL && lookup_type->kind == TYPE_SLICE &&
        strcmp(ast->member.member, "len") == 0) {
        return lower_make_field_access(
            low, &(FieldAccessSpec){object, "len", ast->type, via_ptr, ast->loc});
    }

    // Struct field access
    if (lookup_type != NULL && lookup_type->kind == TYPE_STRUCT) {
        const char *field_name = ast->member.member;

        if (!via_ptr && low->current_recv != NULL && low->current_is_ptr_recv &&
            object->kind == HIR_VAR_REF && object->var_ref.sym == low->current_recv) {
            via_ptr = true;
        }

        // Direct field
        const StructField *sf = type_struct_find_field(lookup_type, field_name);
        if (sf != NULL) {
            return lower_make_field_access(
                low, &(FieldAccessSpec){object, field_name, ast->type, via_ptr, ast->loc});
        }

        // Promoted field from embedded struct
        FieldLookup lookup = {object, lookup_type, field_name, via_ptr, ast->loc};
        HirNode *promoted = lower_resolve_promoted_field(low, &lookup);
        if (promoted != NULL) {
            return promoted;
        }

        // Fallback (shouldn't reach after sema)
        return lower_make_field_access(
            low, &(FieldAccessSpec){object, field_name, ast->type, via_ptr, ast->loc});
    }

    // Enum variant access: EnumType.Variant → enum init
    if (lookup_type != NULL && lookup_type->kind == TYPE_ENUM) {
        return lower_enum_unit_init(low,
                                    &(EnumVariantSpec){lookup_type, ast->member.member, ast->loc});
    }

    // Module access
    HirNode *node = hir_new(low->hir_arena, HIR_MODULE_ACCESS, ast->type, ast->loc);
    node->module_access.object = object;
    node->module_access.member = ast->member.member;
    return node;
}

/**
 * Start an enum variant init: look up the variant, prepare _tag.
 *
 * Returns a state with variant, field_names and field_values bufs
 * (with _tag prepopulated).  Returns variant==NULL on lookup failure.
 */
static EnumInitState begin_enum_init(Lower *low, const EnumVariantSpec *spec) {
    EnumInitState state = {0};
    state.variant = type_enum_find_variant(spec->enum_type, spec->variant_name);
    if (state.variant == NULL) {
        return state;
    }

    BUF_PUSH(state.field_names, "_tag");
    BUF_PUSH(state.field_values,
             lower_make_int_lit(low, &(IntLitSpec){(uint64_t)state.variant->discriminant,
                                                   &TYPE_I32_INST, TYPE_I32, spec->loc}));
    return state;
}

/** Finish an enum variant init: build the HIR_STRUCT_LIT node. */
static HirNode *finish_enum_init(Lower *low, const EnumVariantSpec *spec,
                                 const EnumInitState *state) {
    HirNode *node = hir_new(low->hir_arena, HIR_STRUCT_LIT, spec->enum_type, spec->loc);
    node->struct_lit.field_names = state->field_names;
    node->struct_lit.field_values = state->field_values;

    BUF_PUSH(low->compound_types, spec->enum_type);
    return node;
}

/** Lower an enum unit variant access: Enum.Variant → struct lit with _tag. */
static HirNode *lower_enum_unit_init(Lower *low, const EnumVariantSpec *spec) {
    EnumInitState state = begin_enum_init(low, spec);
    if (state.variant == NULL) {
        return hir_new(low->hir_arena, HIR_UNIT_LIT, &TYPE_ERR_INST, spec->loc);
    }
    return finish_enum_init(low, spec, &state);
}

/** Lower an enum tuple variant call: Enum.Variant(args) → struct lit with _tag + data. */
static HirNode *lower_enum_tuple_init(Lower *low, const EnumVariantSpec *spec, ASTNode **args) {
    EnumInitState state = begin_enum_init(low, spec);
    if (state.variant == NULL) {
        return hir_new(low->hir_arena, HIR_UNIT_LIT, &TYPE_ERR_INST, spec->loc);
    }

    for (int32_t i = 0; i < BUF_LEN(args); i++) {
        // Skip unit-typed fields — they have no C representation
        if (state.variant->tuple_types[i]->kind == TYPE_UNIT) {
            continue;
        }
        HirNode *val = lower_expr(low, args[i]);
        const char *fname = arena_sprintf(low->hir_arena, "_data.%s._%d", spec->variant_name, i);
        BUF_PUSH(state.field_names, fname);
        BUF_PUSH(state.field_values, val);
    }
    return finish_enum_init(low, spec, &state);
}

/** Lower an enum struct variant init: Enum.Variant { field = val } → struct lit. */
static HirNode *lower_enum_struct_init(Lower *low, const EnumVariantSpec *spec,
                                       const char **ast_field_names, ASTNode **ast_field_values) {
    EnumInitState state = begin_enum_init(low, spec);
    if (state.variant == NULL) {
        return hir_new(low->hir_arena, HIR_UNIT_LIT, &TYPE_ERR_INST, spec->loc);
    }

    for (int32_t i = 0; i < BUF_LEN(ast_field_names); i++) {
        const char *fname =
            arena_sprintf(low->hir_arena, "_data.%s.%s", spec->variant_name, ast_field_names[i]);
        BUF_PUSH(state.field_names, fname);
        BUF_PUSH(state.field_values, lower_expr(low, ast_field_values[i]));
    }
    return finish_enum_init(low, spec, &state);
}

static HirNode *lower_idx(Lower *low, const ASTNode *ast) {
    HirNode *object = lower_expr(low, ast->idx_access.object);
    HirNode *idx = lower_expr(low, ast->idx_access.idx);
    HirNode *node = hir_new(low->hir_arena, HIR_IDX, ast->type, ast->loc);
    node->idx_access.object = object;
    node->idx_access.idx = idx;
    return node;
}

static HirNode *lower_type_conversion(Lower *low, const ASTNode *ast) {
    HirNode *operand = lower_expr(low, ast->type_conversion.operand);
    HirNode *node = hir_new(low->hir_arena, HIR_TYPE_CONVERSION, ast->type, ast->loc);
    node->type_conversion.operand = operand;
    node->type_conversion.target_type = ast->type;
    return node;
}

static HirNode *lower_array_lit(Lower *low, const ASTNode *ast) {
    HirNode **elems = lower_elem_list(low, ast->array_lit.elems);
    HirNode *node = hir_new(low->hir_arena, HIR_ARRAY_LIT, ast->type, ast->loc);
    node->array_lit.elems = elems;
    return node;
}

static HirNode *lower_slice_lit(Lower *low, const ASTNode *ast) {
    HirNode **elems = lower_elem_list(low, ast->slice_lit.elems);
    HirNode *node = hir_new(low->hir_arena, HIR_SLICE_LIT, ast->type, ast->loc);
    node->slice_lit.elems = elems;
    return node;
}

static HirNode *lower_slice_expr(Lower *low, const ASTNode *ast) {
    HirNode *object = lower_expr(low, ast->slice_expr.object);
    HirNode *start = ast->slice_expr.start != NULL ? lower_expr(low, ast->slice_expr.start) : NULL;
    HirNode *end = ast->slice_expr.end != NULL ? lower_expr(low, ast->slice_expr.end) : NULL;
    bool from_array = object->type != NULL && object->type->kind == TYPE_ARRAY;
    HirNode *node = hir_new(low->hir_arena, HIR_SLICE_EXPR, ast->type, ast->loc);
    node->slice_expr.object = object;
    node->slice_expr.start = start;
    node->slice_expr.end = end;
    node->slice_expr.from_array = from_array;
    return node;
}

static HirNode *lower_tuple_lit(Lower *low, const ASTNode *ast) {
    HirNode **elems = lower_elem_list(low, ast->tuple_lit.elems);
    HirNode *node = hir_new(low->hir_arena, HIR_TUPLE_LIT, ast->type, ast->loc);
    node->tuple_lit.elems = elems;
    return node;
}

// Str interpolation lower is in lower_str.c

/** Find a field value in an AST struct lit by name.  Returns the AST idx or -1. */
static int32_t find_ast_field_idx(const ASTNode *ast, const char *name) {
    for (int32_t i = 0; i < BUF_LEN(ast->struct_lit.field_names); i++) {
        if (strcmp(ast->struct_lit.field_names[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

/** Collect AST-level promoted fields that belong to @p embed_type into a sub-lit. */
static HirNode *collect_promoted_fields(Lower *low, const ASTNode *ast, const Type *embed_type) {
    const char **sub_names = NULL;
    HirNode **sub_values = NULL;

    for (int32_t i = 0; i < embed_type->struct_type.field_count; i++) {
        const char *embed_field = embed_type->struct_type.fields[i].name;
        int32_t ai = find_ast_field_idx(ast, embed_field);
        if (ai >= 0) {
            BUF_PUSH(sub_names, embed_field);
            BUF_PUSH(sub_values, lower_expr(low, ast->struct_lit.field_values[ai]));
        }
    }

    if (BUF_LEN(sub_names) == 0) {
        return NULL;
    }
    HirNode *sub = hir_new(low->hir_arena, HIR_STRUCT_LIT, embed_type, ast->loc);
    sub->struct_lit.field_names = sub_names;
    sub->struct_lit.field_values = sub_values;
    return sub;
}

static HirNode *lower_struct_lit(Lower *low, const ASTNode *ast) {
    const Type *struct_type = ast->type;
    if (struct_type == NULL || struct_type->kind != TYPE_STRUCT) {
        return hir_new(low->hir_arena, HIR_UNIT_LIT, &TYPE_ERR_INST, ast->loc);
    }

    const char **field_names = NULL;
    HirNode **field_values = NULL;

    for (int32_t fi = 0; fi < struct_type->struct_type.field_count; fi++) {
        const StructField *sf = &struct_type->struct_type.fields[fi];

        // Check if this field is an embedded struct
        bool is_embedded = false;
        for (int32_t ei = 0; ei < struct_type->struct_type.embed_count; ei++) {
            if (strcmp(struct_type->struct_type.embedded[ei]->struct_type.name, sf->name) == 0) {
                is_embedded = true;
                break;
            }
        }

        if (is_embedded && sf->type->kind == TYPE_STRUCT) {
            int32_t ai = find_ast_field_idx(ast, sf->name);
            if (ai >= 0) {
                BUF_PUSH(field_names, sf->name);
                BUF_PUSH(field_values, lower_expr(low, ast->struct_lit.field_values[ai]));
            } else {
                HirNode *sub = collect_promoted_fields(low, ast, sf->type);
                if (sub != NULL) {
                    BUF_PUSH(field_names, sf->name);
                    BUF_PUSH(field_values, sub);
                }
            }
        } else {
            int32_t ai = find_ast_field_idx(ast, sf->name);
            if (ai >= 0) {
                BUF_PUSH(field_names, sf->name);
                BUF_PUSH(field_values, lower_expr(low, ast->struct_lit.field_values[ai]));
            }
        }
    }

    HirNode *node = hir_new(low->hir_arena, HIR_STRUCT_LIT, struct_type, ast->loc);
    node->struct_lit.field_names = field_names;
    node->struct_lit.field_values = field_values;
    return node;
}

/**
 * Desugar `expr?.member` → block that checks Some/None and wraps result.
 *
 * { var __tmp = expr;
 *   if __tmp._tag == SOME_TAG {
 *       Some(__tmp._data.Some._0.member)
 *   } else { None } }
 */
static HirNode *lower_optional_chain(Lower *low, const ASTNode *ast) {
    SrcLoc loc = ast->loc;
    HirNode *object = lower_expr(low, ast->optional_chain.object);
    const Type *obj_type = object->type;
    const Type *result_type = ast->type;

    const char *tmp = lower_make_temp_name(low);
    HirSym *tmp_sym = lower_add_var(low, &(HirSymSpec){HIR_SYM_VAR, tmp, obj_type, false, loc});

    const EnumVariant *some_v = type_enum_find_variant(obj_type, "Some");
    const Type *inner_type = some_v->tuple_types[0];

    const EnumVariant *res_some = type_enum_find_variant(result_type, "Some");
    const EnumVariant *res_none = type_enum_find_variant(result_type, "None");

    // Tag check: __tmp._tag == SOME_TAG
    HirNode *tmp_ref = lower_make_var_ref(low, tmp_sym, loc);
    HirNode *tag = lower_make_field_access(
        low, &(FieldAccessSpec){tmp_ref, "_tag", &TYPE_I32_INST, false, loc});

    HirNode *some_tag = lower_make_int_lit(
        low, &(IntLitSpec){(uint64_t)some_v->discriminant, &TYPE_I32_INST, TYPE_I32, loc});
    HirNode *cond = hir_new(low->hir_arena, HIR_BINARY, &TYPE_BOOL_INST, loc);
    cond->binary.op = TOKEN_EQUAL_EQUAL;
    cond->binary.left = tag;
    cond->binary.right = some_tag;

    // Then: extract inner, access field, wrap in Some
    HirNode *inner =
        lower_make_field_access(low, &(FieldAccessSpec){lower_make_var_ref(low, tmp_sym, loc),
                                                        "_data.Some._0", inner_type, false, loc});

    bool via_ptr = inner_type->kind == TYPE_PTR;
    const Type *deref_type = via_ptr ? type_ptr_pointee(inner_type) : inner_type;
    const StructField *sf = type_struct_find_field(deref_type, ast->optional_chain.member);
    const Type *field_type = sf != NULL ? sf->type : &TYPE_ERR_INST;

    HirNode *field = lower_make_field_access(
        low, &(FieldAccessSpec){inner, ast->optional_chain.member, field_type, via_ptr, loc});

    HirNode *then_val;
    if (type_equal(field_type, result_type)) {
        // Field is already Option — propagate directly (no double-wrap)
        then_val = field;
    } else {
        // Wrap in Some of result type
        const char **sfn = NULL;
        HirNode **sfv = NULL;
        BUF_PUSH(sfn, "_tag");
        BUF_PUSH(sfv, lower_make_int_lit(low, &(IntLitSpec){(uint64_t)res_some->discriminant,
                                                            &TYPE_I32_INST, TYPE_I32, loc}));
        BUF_PUSH(sfn, "_data.Some._0");
        BUF_PUSH(sfv, field);
        then_val = hir_new(low->hir_arena, HIR_STRUCT_LIT, result_type, loc);
        then_val->struct_lit.field_names = sfn;
        then_val->struct_lit.field_values = sfv;
    }
    BUF_PUSH(low->compound_types, result_type);

    // Else: None of result type
    const char **nfn = NULL;
    HirNode **nfv = NULL;
    BUF_PUSH(nfn, "_tag");
    BUF_PUSH(nfv, lower_make_int_lit(low, &(IntLitSpec){(uint64_t)res_none->discriminant,
                                                        &TYPE_I32_INST, TYPE_I32, loc}));
    HirNode *none_lit = hir_new(low->hir_arena, HIR_STRUCT_LIT, result_type, loc);
    none_lit->struct_lit.field_names = nfn;
    none_lit->struct_lit.field_values = nfv;

    // Build if
    HirNode *if_node = hir_new(low->hir_arena, HIR_IF, result_type, loc);
    if_node->if_expr.cond = cond;
    if_node->if_expr.then_body = then_val;
    if_node->if_expr.else_body = none_lit;

    // Wrap in block: { var __tmp = expr; if_node }
    HirNode **stmts = NULL;
    BUF_PUSH(stmts, lower_make_var_decl(low, tmp_sym, object));
    HirNode *block = hir_new(low->hir_arena, HIR_BLOCK, result_type, loc);
    block->block.stmts = stmts;
    block->block.result = if_node;
    return block;
}

/**
 * Desugar `expr!` → extract Ok value or return Err from function.
 *
 * { var __tmp = expr;
 *   if __tmp._tag == ERR_TAG {
 *       return { _tag: ERR_TAG, _data.Err._0: __tmp._data.Err._0 };
 *   }
 *   __tmp._data.Ok._0 }
 */
static HirNode *lower_try_expr(Lower *low, const ASTNode *ast) {
    SrcLoc loc = ast->loc;
    HirNode *operand = lower_expr(low, ast->try_expr.operand);
    const Type *op_type = operand->type;
    const Type *ok_type = ast->type;

    const char *tmp = lower_make_temp_name(low);
    HirSym *tmp_sym = lower_add_var(low, &(HirSymSpec){HIR_SYM_VAR, tmp, op_type, false, loc});

    const EnumVariant *err_v = type_enum_find_variant(op_type, "Err");

    // Tag check: __tmp._tag == ERR_TAG
    HirNode *tmp_ref = lower_make_var_ref(low, tmp_sym, loc);
    HirNode *tag = lower_make_field_access(
        low, &(FieldAccessSpec){tmp_ref, "_tag", &TYPE_I32_INST, false, loc});

    HirNode *err_tag = lower_make_int_lit(
        low, &(IntLitSpec){(uint64_t)err_v->discriminant, &TYPE_I32_INST, TYPE_I32, loc});
    HirNode *cond = hir_new(low->hir_arena, HIR_BINARY, &TYPE_BOOL_INST, loc);
    cond->binary.op = TOKEN_EQUAL_EQUAL;
    cond->binary.left = tag;
    cond->binary.right = err_tag;

    // Extract err data: __tmp._data.Err._0
    const Type *err_type = err_v->tuple_count > 0 ? err_v->tuple_types[0] : &TYPE_UNIT_INST;
    HirNode *err_data =
        lower_make_field_access(low, &(FieldAccessSpec){lower_make_var_ref(low, tmp_sym, loc),
                                                        "_data.Err._0", err_type, false, loc});

    // Build Err return value using the fn return type
    const Type *ret_type = low->fn_return_type;
    const EnumVariant *ret_err = type_enum_find_variant(ret_type, "Err");

    const char **efn = NULL;
    HirNode **efv = NULL;
    BUF_PUSH(efn, "_tag");
    BUF_PUSH(efv, lower_make_int_lit(low, &(IntLitSpec){(uint64_t)ret_err->discriminant,
                                                        &TYPE_I32_INST, TYPE_I32, loc}));
    BUF_PUSH(efn, "_data.Err._0");
    BUF_PUSH(efv, err_data);
    HirNode *err_lit = hir_new(low->hir_arena, HIR_STRUCT_LIT, ret_type, loc);
    err_lit->struct_lit.field_names = efn;
    err_lit->struct_lit.field_values = efv;
    BUF_PUSH(low->compound_types, ret_type);

    // Return the Err
    HirNode *ret = hir_new(low->hir_arena, HIR_RETURN, ret_type, loc);
    ret->return_stmt.value = err_lit;

    HirNode **ret_stmts = NULL;
    BUF_PUSH(ret_stmts, ret);
    HirNode *ret_block = hir_new(low->hir_arena, HIR_BLOCK, &TYPE_UNIT_INST, loc);
    ret_block->block.stmts = ret_stmts;
    ret_block->block.result = NULL;

    HirNode *guard = hir_new(low->hir_arena, HIR_IF, &TYPE_UNIT_INST, loc);
    guard->if_expr.cond = cond;
    guard->if_expr.then_body = ret_block;
    guard->if_expr.else_body = NULL;

    // Extract ok data: __tmp._data.Ok._0
    HirNode *ok_data =
        lower_make_field_access(low, &(FieldAccessSpec){lower_make_var_ref(low, tmp_sym, loc),
                                                        "_data.Ok._0", ok_type, false, loc});

    // Block: { var __tmp = expr; if err { return Err }; __tmp._data.Ok._0 }
    HirNode **stmts = NULL;
    BUF_PUSH(stmts, lower_make_var_decl(low, tmp_sym, operand));
    BUF_PUSH(stmts, guard);
    HirNode *block = hir_new(low->hir_arena, HIR_BLOCK, ok_type, loc);
    block->block.stmts = stmts;
    block->block.result = ok_data;
    return block;
}

/** Recursively scan AST for NODE_ID references that aren't closure params. */
static void scan_captures(const ASTNode *ast, const char **param_names, int32_t param_count,
                          Lower *low, const char ***out_names, HirSym ***out_syms) {
    if (ast == NULL) {
        return;
    }
    if (ast->kind == NODE_ID) {
        const char *name = ast->id.name;
        // Skip if it's a closure param
        for (int32_t i = 0; i < param_count; i++) {
            if (strcmp(name, param_names[i]) == 0) {
                return;
            }
        }
        // Skip if already captured
        for (int32_t i = 0; i < BUF_LEN(*out_names); i++) {
            if (strcmp(name, (*out_syms)[i]->name) == 0) {
                return;
            }
        }
        // Check if it refers to a local variable in enclosing scope
        HirSym *sym = lower_scope_lookup(low, name);
        if (sym != NULL && (sym->kind == HIR_SYM_VAR || sym->kind == HIR_SYM_PARAM)) {
            BUF_PUSH(*out_names, sym->mangled_name);
            BUF_PUSH(*out_syms, sym);
        }
        return;
    }
    if (ast->kind == NODE_CLOSURE) {
        return; // Don't scan into nested closures
    }
    // Recurse into children based on node kind
    switch (ast->kind) {
    case NODE_UNARY:
        scan_captures(ast->unary.operand, param_names, param_count, low, out_names, out_syms);
        break;
    case NODE_BINARY:
        scan_captures(ast->binary.left, param_names, param_count, low, out_names, out_syms);
        scan_captures(ast->binary.right, param_names, param_count, low, out_names, out_syms);
        break;
    case NODE_CALL:
        scan_captures(ast->call.callee, param_names, param_count, low, out_names, out_syms);
        for (int32_t i = 0; i < BUF_LEN(ast->call.args); i++) {
            scan_captures(ast->call.args[i], param_names, param_count, low, out_names, out_syms);
        }
        break;
    case NODE_BLOCK:
        for (int32_t i = 0; i < BUF_LEN(ast->block.stmts); i++) {
            scan_captures(ast->block.stmts[i], param_names, param_count, low, out_names, out_syms);
        }
        if (ast->block.result != NULL) {
            scan_captures(ast->block.result, param_names, param_count, low, out_names, out_syms);
        }
        break;
    case NODE_IF:
        scan_captures(ast->if_expr.cond, param_names, param_count, low, out_names, out_syms);
        scan_captures(ast->if_expr.then_body, param_names, param_count, low, out_names, out_syms);
        scan_captures(ast->if_expr.else_body, param_names, param_count, low, out_names, out_syms);
        break;
    case NODE_RETURN:
        scan_captures(ast->return_stmt.value, param_names, param_count, low, out_names, out_syms);
        break;
    case NODE_VAR_DECL:
        scan_captures(ast->var_decl.init, param_names, param_count, low, out_names, out_syms);
        break;
    case NODE_EXPR_STMT:
        scan_captures(ast->expr_stmt.expr, param_names, param_count, low, out_names, out_syms);
        break;
    case NODE_MEMBER:
        scan_captures(ast->member.object, param_names, param_count, low, out_names, out_syms);
        break;
    case NODE_IDX:
        scan_captures(ast->idx_access.object, param_names, param_count, low, out_names, out_syms);
        scan_captures(ast->idx_access.idx, param_names, param_count, low, out_names, out_syms);
        break;
    case NODE_ASSIGN:
        scan_captures(ast->assign.target, param_names, param_count, low, out_names, out_syms);
        scan_captures(ast->assign.value, param_names, param_count, low, out_names, out_syms);
        break;
    case NODE_LIT: // NOLINT(bugprone-branch-clone)
    case NODE_STR_INTERPOLATION:
        break;
    default:
        break;
    }
}

static HirNode *lower_closure(Lower *low, const ASTNode *ast) {
    SrcLoc loc = ast->loc;
    const Type *fn_type = ast->type;

    // Generate a unique name for the closure function
    const char *fn_name = arena_sprintf(low->hir_arena, "_rsg_closure_%d", low->closure_counter++);

    // Collect closure param names for capture scanner
    int32_t param_count = BUF_LEN(ast->closure.params);
    const char **param_names = NULL;
    for (int32_t i = 0; i < param_count; i++) {
        BUF_PUSH(param_names, ast->closure.params[i]->param.name);
    }

    // Scan for captured variables
    const char **capture_names = NULL;
    HirSym **capture_syms = NULL;
    scan_captures(ast->closure.body, param_names, param_count, low, &capture_names, &capture_syms);

    // Lower closure params to HIR_PARAM
    HirNode **hir_params = NULL;
    lower_scope_enter(low);
    for (int32_t i = 0; i < param_count; i++) {
        const ASTNode *p = ast->closure.params[i];
        HirSymSpec spec = {HIR_SYM_PARAM, p->param.name, p->type, p->param.is_mut, p->loc};
        HirSym *sym = lower_make_sym(low, &spec);
        lower_scope_define(low, p->param.name, sym);
        HirNode *hir_param = hir_new(low->hir_arena, HIR_PARAM, p->type, p->loc);
        hir_param->param.sym = sym;
        BUF_PUSH(hir_params, hir_param);
    }

    // Define captured vars in closure scope so body can reference them
    for (int32_t i = 0; i < BUF_LEN(capture_syms); i++) {
        lower_scope_define(low, capture_syms[i]->name, capture_syms[i]);
    }

    // Lower closure body
    HirNode *body = lower_expr(low, ast->closure.body);
    lower_scope_leave(low);

    // Build HIR_CLOSURE
    const Type *return_type = fn_type->fn_type.return_type;
    HirNode *node = hir_new(low->hir_arena, HIR_CLOSURE, fn_type, loc);
    node->closure.fn_name = fn_name;
    node->closure.params = hir_params;
    node->closure.body = body;
    node->closure.capture_names = capture_names;
    node->closure.capture_syms = capture_syms;
    node->closure.return_type = return_type;
    node->closure.is_fn_mut = (fn_type->fn_type.fn_kind == FN_CLOSURE_MUT);

    BUF_FREE(param_names);
    return node;
}

HirNode *lower_expr(Lower *low, const ASTNode *ast) {
    if (ast == NULL) {
        return NULL;
    }
    switch (ast->kind) {
    case NODE_LIT:
        return lower_lit(low, ast);
    case NODE_ID:
        return lower_id(low, ast);
    case NODE_UNARY:
        return lower_unary(low, ast);
    case NODE_BINARY:
        return lower_binary(low, ast);
    case NODE_CALL:
        return lower_call(low, ast);
    case NODE_MEMBER:
        return lower_member(low, ast);
    case NODE_IDX:
        return lower_idx(low, ast);
    case NODE_IF:
        return lower_stmt_if(low, ast);
    case NODE_BLOCK:
        return lower_block(low, ast);
    case NODE_TYPE_CONVERSION:
        return lower_type_conversion(low, ast);
    case NODE_ARRAY_LIT:
        return lower_array_lit(low, ast);
    case NODE_SLICE_LIT:
        return lower_slice_lit(low, ast);
    case NODE_SLICE_EXPR:
        return lower_slice_expr(low, ast);
    case NODE_TUPLE_LIT:
        return lower_tuple_lit(low, ast);
    case NODE_STR_INTERPOLATION:
        return lower_str_interpolation(low, ast);
    case NODE_STRUCT_LIT:
        return lower_struct_lit(low, ast);
    case NODE_ADDRESS_OF: {
        HirNode *operand = lower_expr(low, ast->address_of.operand);
        // &StructLit{} / &SliceLit{} / &ArrayLit{} → heap alloc
        if (ast->address_of.operand->kind == NODE_STRUCT_LIT ||
            ast->address_of.operand->kind == NODE_SLICE_LIT ||
            ast->address_of.operand->kind == NODE_ARRAY_LIT) {
            HirNode *node = hir_new(low->hir_arena, HIR_HEAP_ALLOC, ast->type, ast->loc);
            node->heap_alloc.operand = operand;
            return node;
        }
        // &var → native address-of
        HirNode *node = hir_new(low->hir_arena, HIR_ADDRESS_OF, ast->type, ast->loc);
        node->address_of.operand = operand;
        return node;
    }
    case NODE_DEREF: {
        HirNode *operand = lower_expr(low, ast->deref.operand);
        HirNode *node = hir_new(low->hir_arena, HIR_DEREF, ast->type, ast->loc);
        node->deref.operand = operand;
        return node;
    }
    case NODE_MATCH:
        return lower_match(low, ast);
    case NODE_LOOP:
        return lower_node(low, ast);
    case NODE_ENUM_INIT: {
        EnumVariantSpec spec = {ast->type, ast->enum_init.variant_name, ast->loc};
        return lower_enum_struct_init(low, &spec, ast->enum_init.field_names,
                                      ast->enum_init.field_values);
    }
    case NODE_OPTIONAL_CHAIN:
        return lower_optional_chain(low, ast);
    case NODE_TRY:
        return lower_try_expr(low, ast);
    case NODE_CLOSURE:
        return lower_closure(low, ast);
    default:
        rsg_fatal("lower_expr: unhandled AST node kind %d", (int)ast->kind);
    }
}
