#include "_lowering.h"

// ── Expression lowering ───────────────────────────────────────────────

static TTNode *lower_lit(Lowering *low, const ASTNode *ast) {
    SourceLoc loc = ast->loc;
    const Type *type = ast->type;

    switch (ast->lit.kind) {
    case LIT_BOOL: {
        TTNode *node = tt_new(low->tt_arena, TT_BOOL_LIT, type, loc);
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
        TTNode *node = tt_new(low->tt_arena, TT_INT_LIT, type, loc);
        node->int_lit.value = ast->lit.integer_value;
        node->int_lit.int_kind = (TypeKind)ast->lit.kind;
        return node;
    }
    case LIT_F32:
    case LIT_F64: {
        TTNode *node = tt_new(low->tt_arena, TT_FLOAT_LIT, type, loc);
        node->float_lit.value = ast->lit.float64_value;
        node->float_lit.float_kind = (TypeKind)ast->lit.kind;
        return node;
    }
    case LIT_CHAR: {
        TTNode *node = tt_new(low->tt_arena, TT_CHAR_LIT, type, loc);
        node->char_lit.value = ast->lit.char_value;
        return node;
    }
    case LIT_STR: {
        TTNode *node = tt_new(low->tt_arena, TT_STR_LIT, type, loc);
        node->str_lit.value = ast->lit.str_value;
        return node;
    }
    case LIT_UNIT:
        return tt_new(low->tt_arena, TT_UNIT_LIT, type, loc);
    }
    return tt_new(low->tt_arena, TT_UNIT_LIT, &TYPE_ERR_INST, loc);
}

static TTNode *lower_id(Lowering *low, const ASTNode *ast) {
    const char *name = ast->id.name;
    TTSym *sym = lowering_scope_find(low, name);
    if (sym == NULL) {
        // Create an unresolved sym with whatever type sema assigned.
        TtSymSpec id_spec = {TT_SYM_VAR, name, ast->type, false, ast->loc};
        sym = lowering_add_var(low, &id_spec);
    }
    return lowering_make_var_ref(low, sym, ast->loc);
}

static TTNode *lower_unary(Lowering *low, const ASTNode *ast) {
    TTNode *operand = lower_expr(low, ast->unary.operand);
    TTNode *node = tt_new(low->tt_arena, TT_UNARY, ast->type, ast->loc);
    node->unary.op = ast->unary.op;
    node->unary.operand = operand;
    return node;
}

/** Negate a boolean expr with a unary `!`. */
static TTNode *make_bool_negate(Lowering *low, TTNode *expr, SourceLoc loc) {
    TTNode *neg = tt_new(low->tt_arena, TT_UNARY, &TYPE_BOOL_INST, loc);
    neg->unary.op = TOKEN_BANG;
    neg->unary.operand = expr;
    return neg;
}

/**
 * Build an elem comparison for equality expansion.
 *
 * For TYPE_STR elems, emits rsg_str_equal(l, r) (negated for !=).
 * Otherwise emits a plain binary comparison.
 */
static TTNode *make_elem_comparison(Lowering *low, TTNode *l_elem, TTNode *r_elem,
                                    TokenKind elem_op) {
    SourceLoc loc = l_elem->loc;
    const Type *elem_type = l_elem->type;
    if (elem_type->kind == TYPE_STR) {
        TTNode **args = NULL;
        BUF_PUSH(args, l_elem);
        BUF_PUSH(args, r_elem);
        TTNode *cmp = lowering_make_builtin_call(
            low, &(BuiltinCallSpec){"rsg_str_equal", &TYPE_BOOL_INST, args, loc});
        if (elem_op != TOKEN_EQUAL_EQUAL) {
            cmp = make_bool_negate(low, cmp, loc);
        }
        return cmp;
    }
    TTNode *cmp = tt_new(low->tt_arena, TT_BINARY, &TYPE_BOOL_INST, loc);
    cmp->binary.op = elem_op;
    cmp->binary.left = l_elem;
    cmp->binary.right = r_elem;
    return cmp;
}

/** Join @p cmp into the running @p result chain with @p join_op. */
static TTNode *chain_comparison(Lowering *low, TTNode *result, TTNode *cmp, TokenKind join_op,
                                SourceLoc loc) {
    if (result == NULL) {
        return cmp;
    }
    TTNode *join = tt_new(low->tt_arena, TT_BINARY, &TYPE_BOOL_INST, loc);
    join->binary.op = join_op;
    join->binary.left = result;
    join->binary.right = cmp;
    return join;
}

/** Build an elem access: array idx or tuple idx depending on @p kind. */
static TTNode *make_elem_access(Lowering *low, TTSym *sym, int32_t idx, const Type *elem_type,
                                TtNodeKind kind, SourceLoc loc) {
    TTNode *ref = lowering_make_var_ref(low, sym, loc);
    if (kind == TT_IDX) {
        IntLitSpec idx_spec = {(uint64_t)idx, &TYPE_I32_INST, TYPE_I32, loc};
        TTNode *idx = lowering_make_int_lit(low, &idx_spec);
        TTNode *elem = tt_new(low->tt_arena, TT_IDX, elem_type, loc);
        elem->idx_access.object = ref;
        elem->idx_access.idx = idx;
        return elem;
    }
    TTNode *elem = tt_new(low->tt_arena, TT_TUPLE_IDX, elem_type, loc);
    elem->tuple_idx.object = ref;
    elem->tuple_idx.elem_idx = idx;
    return elem;
}

/**
 * Expand compound (array/tuple) equality:
 * `a == b` → `{ var l=a; var r=b; l[0]==r[0] && l[1]==r[1] && ... }`
 */
static TTNode *lower_compound_equality(Lowering *low, TTNode *left, TTNode *right, TokenKind op,
                                       SourceLoc loc) {
    const Type *type = left->type;
    bool is_array = (type->kind == TYPE_ARRAY);
    int32_t count = is_array ? type->array.size : type->tuple.count;
    bool is_equal = (op == TOKEN_EQUAL_EQUAL);
    TokenKind elem_op = is_equal ? TOKEN_EQUAL_EQUAL : TOKEN_BANG_EQUAL;
    TokenKind join_op = is_equal ? TOKEN_AMPERSAND_AMPERSAND : TOKEN_PIPE_PIPE;

    const char *left_name = lowering_make_temp_name(low);
    TtSymSpec left_spec = {TT_SYM_VAR, left_name, type, false, loc};
    TTSym *left_sym = lowering_add_var(low, &left_spec);

    const char *right_name = lowering_make_temp_name(low);
    TtSymSpec right_spec = {TT_SYM_VAR, right_name, type, false, loc};
    TTSym *right_sym = lowering_add_var(low, &right_spec);

    TTNode *result = NULL;
    TtNodeKind access_kind = is_array ? TT_IDX : TT_TUPLE_IDX;
    for (int32_t i = 0; i < count; i++) {
        const Type *elem_type = is_array ? type->array.elem : type->tuple.elems[i];
        TTNode *l_elem = make_elem_access(low, left_sym, i, elem_type, access_kind, loc);
        TTNode *r_elem = make_elem_access(low, right_sym, i, elem_type, access_kind, loc);
        TTNode *cmp = make_elem_comparison(low, l_elem, r_elem, elem_op);
        result = chain_comparison(low, result, cmp, join_op, loc);
    }

    if (result == NULL) {
        TTNode *lit = tt_new(low->tt_arena, TT_BOOL_LIT, &TYPE_BOOL_INST, loc);
        lit->bool_lit.value = is_equal;
        return lit;
    }

    TTNode **stmts = NULL;
    BUF_PUSH(stmts, lowering_make_var_decl(low, left_sym, left));
    BUF_PUSH(stmts, lowering_make_var_decl(low, right_sym, right));

    TTNode *block = tt_new(low->tt_arena, TT_BLOCK, &TYPE_BOOL_INST, loc);
    block->block.stmts = stmts;
    block->block.result = result;
    return block;
}

/** Lower str equality: rsg_str_equal(l, r), negated for !=. */
static TTNode *lower_str_equality(Lowering *low, TTNode *left, TTNode *right, TokenKind op,
                                  SourceLoc loc) {
    TTNode **args = NULL;
    BUF_PUSH(args, left);
    BUF_PUSH(args, right);
    TTNode *call = lowering_make_builtin_call(
        low, &(BuiltinCallSpec){"rsg_str_equal", &TYPE_BOOL_INST, args, loc});
    if (op == TOKEN_BANG_EQUAL) {
        return make_bool_negate(low, call, loc);
    }
    return call;
}

static TTNode *lower_binary(Lowering *low, const ASTNode *ast) {
    TTNode *left = lower_expr(low, ast->binary.left);
    TTNode *right = lower_expr(low, ast->binary.right);
    TokenKind op = ast->binary.op;
    const Type *left_type = left->type;

    // Str equality/inequality → rsg_str_equal call
    if (left_type != NULL && left_type->kind == TYPE_STR &&
        (op == TOKEN_EQUAL_EQUAL || op == TOKEN_BANG_EQUAL)) {
        return lower_str_equality(low, left, right, op, ast->loc);
    }

    // Unit equality/inequality → constant true/false
    if (left_type != NULL && left_type->kind == TYPE_UNIT &&
        (op == TOKEN_EQUAL_EQUAL || op == TOKEN_BANG_EQUAL)) {
        TTNode *lit = tt_new(low->tt_arena, TT_BOOL_LIT, &TYPE_BOOL_INST, ast->loc);
        lit->bool_lit.value = (op == TOKEN_EQUAL_EQUAL);
        return lit;
    }

    // Array/tuple equality/inequality → elem-wise comparison
    if (left_type != NULL && (left_type->kind == TYPE_ARRAY || left_type->kind == TYPE_TUPLE) &&
        (op == TOKEN_EQUAL_EQUAL || op == TOKEN_BANG_EQUAL)) {
        return lower_compound_equality(low, left, right, op, ast->loc);
    }

    TTNode *node = tt_new(low->tt_arena, TT_BINARY, ast->type, ast->loc);
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

/**
 * Expand assert(cond, msg?) → rsg_assert(cond, msg_or_null, file, line).
 *
 * When the msg is a str lit, pass its raw value directly as a
 * TT_STR_LIT so codegen can emit a plain C str.  For non-lit
 * str exprs, lowering wraps them unchanged — codegen extracts
 * `.data` from the emitted rsg_str.
 */
static TTNode *lower_assert_call(Lowering *low, const ASTNode *ast) {
    int32_t arg_count = BUF_LEN(ast->call.args);
    SourceLoc loc = ast->loc;

    // Condition (default to false if missing)
    TTNode *cond;
    if (arg_count > 0) {
        cond = lower_expr(low, ast->call.args[0]);
    } else {
        cond = tt_new(low->tt_arena, TT_BOOL_LIT, &TYPE_BOOL_INST, loc);
        cond->bool_lit.value = false;
    }

    // Message (NULL when absent; str lit kept as lit, expr passed through)
    TTNode *msg;
    if (arg_count > 1) {
        msg = lower_expr(low, ast->call.args[1]);
    } else {
        // Pass NULL sentinel — codegen emits "NULL" for this
        msg = tt_new(low->tt_arena, TT_UNIT_LIT, &TYPE_UNIT_INST, loc);
    }

    // File name as str lit
    TTNode *file_node = tt_new(low->tt_arena, TT_STR_LIT, &TYPE_STR_INST, loc);
    file_node->str_lit.value = loc.file != NULL ? loc.file : "<unknown>";

    // Line number as i32 lit
    IntLitSpec line_spec = {(uint64_t)loc.line, &TYPE_I32_INST, TYPE_I32, loc};
    TTNode *line_node = lowering_make_int_lit(low, &line_spec);

    TTNode **args = NULL;
    BUF_PUSH(args, cond);
    BUF_PUSH(args, msg);
    BUF_PUSH(args, file_node);
    BUF_PUSH(args, line_node);

    return lowering_make_builtin_call(low,
                                      &(BuiltinCallSpec){"rsg_assert", &TYPE_UNIT_INST, args, loc});
}

/** Lower a buf of AST expr nodes into a buf of TT nodes. */
static TTNode **lower_elem_list(Lowering *low, ASTNode **ast_elems) {
    TTNode **elems = NULL;
    for (int32_t i = 0; i < BUF_LEN(ast_elems); i++) {
        BUF_PUSH(elems, lower_expr(low, ast_elems[i]));
    }
    return elems;
}

// Forward decls for enum init helpers
typedef struct {
    const Type *enum_type;
    const char *variant_name;
    SourceLoc loc;
} EnumVariantSpec;

typedef struct {
    const EnumVariant *variant;
    const char **field_names;
    TTNode **field_values;
} EnumInitState;

static TTNode *lower_enum_unit_init(Lowering *low, const EnumVariantSpec *spec);
static TTNode *lower_enum_tuple_init(Lowering *low, const EnumVariantSpec *spec, ASTNode **args);
static TTNode *lower_enum_struct_init(Lowering *low, const EnumVariantSpec *spec,
                                      const char **ast_field_names, ASTNode **ast_field_values);

/**
 * Try to lower a member-callee call as a method call.
 *
 * Handles enum variant construction, enum method calls, and struct
 * method calls (including promoted methods from embedded structs).
 * Returns NULL when the callee is not a recognized method.
 */
/** Walk embedded structs to find a promoted method sym. */
static TTSym *find_promoted_method(Lowering *low, const Type *struct_type, const char *method_name,
                                   TTNode **recv_ptr, bool via_ptr) {
    for (int32_t i = 0; i < struct_type->struct_type.embed_count; i++) {
        const Type *embed_type = struct_type->struct_type.embedded[i];
        const char *embed_key =
            arena_sprintf(low->tt_arena, "%s.%s", embed_type->struct_type.name, method_name);
        TTSym *method_sym = lowering_scope_find(low, embed_key);
        if (method_sym != NULL) {
            TTNode *embed_access =
                tt_new(low->tt_arena, TT_STRUCT_FIELD_ACCESS, embed_type, (*recv_ptr)->loc);
            embed_access->struct_field_access.object = *recv_ptr;
            embed_access->struct_field_access.field = embed_type->struct_type.name;
            embed_access->struct_field_access.via_ptr = via_ptr;
            *recv_ptr = embed_access;
            return method_sym;
        }
    }
    return NULL;
}

static TTNode *lower_member_call(Lowering *low, const ASTNode *ast) {
    const ASTNode *member_ast = ast->call.callee;
    const Type *obj_type = member_ast->member.object->type;

    bool via_ptr = false;
    if (obj_type != NULL && obj_type->kind == TYPE_PTR) {
        obj_type = obj_type->ptr.pointee;
        via_ptr = true;
    }

    // Enum: tuple variant construction or method call
    if (obj_type != NULL && obj_type->kind == TYPE_ENUM) {
        const char *variant_name = member_ast->member.member;
        const EnumVariant *variant = type_enum_find_variant(obj_type, variant_name);
        if (variant != NULL && variant->kind == ENUM_VARIANT_TUPLE) {
            EnumVariantSpec spec = {obj_type, variant_name, ast->loc};
            return lower_enum_tuple_init(low, &spec, ast->call.args);
        }
        const char *method_key =
            arena_sprintf(low->tt_arena, "%s.%s", type_enum_name(obj_type), variant_name);
        TTSym *method_sym = lowering_scope_find(low, method_key);
        if (method_sym != NULL) {
            TTNode *recv = lower_expr(low, member_ast->member.object);
            TTNode **args = lower_elem_list(low, ast->call.args);
            TTNode *node = tt_new(low->tt_arena, TT_METHOD_CALL, ast->type, ast->loc);
            node->method_call.recv = recv;
            node->method_call.mangled_name = method_sym->mangled_name;
            node->method_call.args = args;
            node->method_call.is_ptr_recv = method_sym->is_ptr_recv;
            return node;
        }
    }

    // Struct: direct method or promoted method from embedded struct
    if (obj_type != NULL && obj_type->kind == TYPE_STRUCT) {
        const char *method_name = member_ast->member.member;
        TTNode *recv = lower_expr(low, member_ast->member.object);

        if (!via_ptr && low->current_recv != NULL && low->current_is_ptr_recv &&
            recv->kind == TT_VAR_REF && recv->var_ref.sym == low->current_recv) {
            via_ptr = true;
        }

        const char *key =
            arena_sprintf(low->tt_arena, "%s.%s", obj_type->struct_type.name, method_name);
        TTSym *method_sym = lowering_scope_find(low, key);

        if (method_sym == NULL) {
            method_sym = find_promoted_method(low, obj_type, method_name, &recv, via_ptr);
        }

        if (method_sym != NULL) {
            TTNode **args = lower_elem_list(low, ast->call.args);
            TTNode *node = tt_new(low->tt_arena, TT_METHOD_CALL, ast->type, ast->loc);
            node->method_call.recv = recv;
            node->method_call.mangled_name = method_sym->mangled_name;
            node->method_call.args = args;
            node->method_call.is_ptr_recv = method_sym->is_ptr_recv;
            return node;
        }
    }

    return NULL;
}

static TTNode *lower_call(Lowering *low, const ASTNode *ast) {
    if (is_assert_callee(ast->call.callee)) {
        return lower_assert_call(low, ast);
    }

    if (ast->call.callee->kind == NODE_MEMBER) {
        TTNode *method = lower_member_call(low, ast);
        if (method != NULL) {
            return method;
        }
    }

    TTNode *callee = lower_expr(low, ast->call.callee);
    TTNode **args = lower_elem_list(low, ast->call.args);
    TTNode *node = tt_new(low->tt_arena, TT_CALL, ast->type, ast->loc);
    node->call.callee = callee;
    node->call.args = args;
    return node;
}

static TTNode *lower_member(Lowering *low, const ASTNode *ast) {
    TTNode *object = lower_expr(low, ast->member.object);

    // Tuple member access: tuple.0, tuple.1, ...
    if (object->type != NULL && object->type->kind == TYPE_TUPLE) {
        char *end = NULL;
        long idx = strtol(ast->member.member, &end, 10);
        if (end != NULL && *end == '\0') {
            TTNode *node = tt_new(low->tt_arena, TT_TUPLE_IDX, ast->type, ast->loc);
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
        TTNode *node = tt_new(low->tt_arena, TT_STRUCT_FIELD_ACCESS, ast->type, ast->loc);
        node->struct_field_access.object = object;
        node->struct_field_access.field = "len";
        node->struct_field_access.via_ptr = via_ptr;
        return node;
    }

    // Struct field access
    if (lookup_type != NULL && lookup_type->kind == TYPE_STRUCT) {
        const char *field_name = ast->member.member;

        if (!via_ptr && low->current_recv != NULL && low->current_is_ptr_recv &&
            object->kind == TT_VAR_REF && object->var_ref.sym == low->current_recv) {
            via_ptr = true;
        }

        // Direct field
        const StructField *sf = type_struct_find_field(lookup_type, field_name);
        if (sf != NULL) {
            TTNode *node = tt_new(low->tt_arena, TT_STRUCT_FIELD_ACCESS, ast->type, ast->loc);
            node->struct_field_access.object = object;
            node->struct_field_access.field = field_name;
            node->struct_field_access.via_ptr = via_ptr;
            return node;
        }

        // Promoted field from embedded struct
        FieldLookup lookup = {object, lookup_type, field_name, via_ptr, ast->loc};
        TTNode *promoted = lowering_resolve_promoted_field(low, &lookup);
        if (promoted != NULL) {
            return promoted;
        }

        // Fallback (shouldn't reach after sema)
        TTNode *node = tt_new(low->tt_arena, TT_STRUCT_FIELD_ACCESS, ast->type, ast->loc);
        node->struct_field_access.object = object;
        node->struct_field_access.field = field_name;
        node->struct_field_access.via_ptr = via_ptr;
        return node;
    }

    // Enum variant access: EnumType.Variant → enum init
    if (lookup_type != NULL && lookup_type->kind == TYPE_ENUM) {
        return lower_enum_unit_init(low,
                                    &(EnumVariantSpec){lookup_type, ast->member.member, ast->loc});
    }

    // Module access
    TTNode *node = tt_new(low->tt_arena, TT_MODULE_ACCESS, ast->type, ast->loc);
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
static EnumInitState begin_enum_init(Lowering *low, const EnumVariantSpec *spec) {
    EnumInitState state = {0};
    state.variant = type_enum_find_variant(spec->enum_type, spec->variant_name);
    if (state.variant == NULL) {
        return state;
    }

    BUF_PUSH(state.field_names, "_tag");
    BUF_PUSH(state.field_values,
             lowering_make_int_lit(low, &(IntLitSpec){(uint64_t)state.variant->discriminant,
                                                      &TYPE_I32_INST, TYPE_I32, spec->loc}));
    return state;
}

/** Finish an enum variant init: build the TT_STRUCT_LIT node. */
static TTNode *finish_enum_init(Lowering *low, const EnumVariantSpec *spec,
                                const EnumInitState *state) {
    TTNode *node = tt_new(low->tt_arena, TT_STRUCT_LIT, spec->enum_type, spec->loc);
    node->struct_lit.field_names = state->field_names;
    node->struct_lit.field_values = state->field_values;

    BUF_PUSH(low->compound_types, spec->enum_type);
    return node;
}

/** Lower an enum unit variant access: Enum.Variant → struct lit with _tag. */
static TTNode *lower_enum_unit_init(Lowering *low, const EnumVariantSpec *spec) {
    EnumInitState state = begin_enum_init(low, spec);
    if (state.variant == NULL) {
        return tt_new(low->tt_arena, TT_UNIT_LIT, &TYPE_ERR_INST, spec->loc);
    }
    return finish_enum_init(low, spec, &state);
}

/** Lower an enum tuple variant call: Enum.Variant(args) → struct lit with _tag + data. */
static TTNode *lower_enum_tuple_init(Lowering *low, const EnumVariantSpec *spec, ASTNode **args) {
    EnumInitState state = begin_enum_init(low, spec);
    if (state.variant == NULL) {
        return tt_new(low->tt_arena, TT_UNIT_LIT, &TYPE_ERR_INST, spec->loc);
    }

    for (int32_t i = 0; i < BUF_LEN(args); i++) {
        const char *fname = arena_sprintf(low->tt_arena, "_data.%s._%d", spec->variant_name, i);
        BUF_PUSH(state.field_names, fname);
        BUF_PUSH(state.field_values, lower_expr(low, args[i]));
    }
    return finish_enum_init(low, spec, &state);
}

/** Lower an enum struct variant init: Enum.Variant { field = val } → struct lit. */
static TTNode *lower_enum_struct_init(Lowering *low, const EnumVariantSpec *spec,
                                      const char **ast_field_names, ASTNode **ast_field_values) {
    EnumInitState state = begin_enum_init(low, spec);
    if (state.variant == NULL) {
        return tt_new(low->tt_arena, TT_UNIT_LIT, &TYPE_ERR_INST, spec->loc);
    }

    for (int32_t i = 0; i < BUF_LEN(ast_field_names); i++) {
        const char *fname =
            arena_sprintf(low->tt_arena, "_data.%s.%s", spec->variant_name, ast_field_names[i]);
        BUF_PUSH(state.field_names, fname);
        BUF_PUSH(state.field_values, lower_expr(low, ast_field_values[i]));
    }
    return finish_enum_init(low, spec, &state);
}

static TTNode *lower_idx(Lowering *low, const ASTNode *ast) {
    TTNode *object = lower_expr(low, ast->idx_access.object);
    TTNode *idx = lower_expr(low, ast->idx_access.idx);
    TTNode *node = tt_new(low->tt_arena, TT_IDX, ast->type, ast->loc);
    node->idx_access.object = object;
    node->idx_access.idx = idx;
    return node;
}

static TTNode *lower_type_conversion(Lowering *low, const ASTNode *ast) {
    TTNode *operand = lower_expr(low, ast->type_conversion.operand);
    TTNode *node = tt_new(low->tt_arena, TT_TYPE_CONVERSION, ast->type, ast->loc);
    node->type_conversion.operand = operand;
    node->type_conversion.target_type = ast->type;
    return node;
}

static TTNode *lower_array_lit(Lowering *low, const ASTNode *ast) {
    TTNode **elems = lower_elem_list(low, ast->array_lit.elems);
    TTNode *node = tt_new(low->tt_arena, TT_ARRAY_LIT, ast->type, ast->loc);
    node->array_lit.elems = elems;
    return node;
}

static TTNode *lower_slice_lit(Lowering *low, const ASTNode *ast) {
    TTNode **elems = lower_elem_list(low, ast->slice_lit.elems);
    TTNode *node = tt_new(low->tt_arena, TT_SLICE_LIT, ast->type, ast->loc);
    node->slice_lit.elems = elems;
    return node;
}

static TTNode *lower_slice_expr(Lowering *low, const ASTNode *ast) {
    TTNode *object = lower_expr(low, ast->slice_expr.object);
    TTNode *start = ast->slice_expr.start != NULL ? lower_expr(low, ast->slice_expr.start) : NULL;
    TTNode *end = ast->slice_expr.end != NULL ? lower_expr(low, ast->slice_expr.end) : NULL;
    bool from_array = object->type != NULL && object->type->kind == TYPE_ARRAY;
    TTNode *node = tt_new(low->tt_arena, TT_SLICE_EXPR, ast->type, ast->loc);
    node->slice_expr.object = object;
    node->slice_expr.start = start;
    node->slice_expr.end = end;
    node->slice_expr.from_array = from_array;
    return node;
}

static TTNode *lower_tuple_lit(Lowering *low, const ASTNode *ast) {
    TTNode **elems = lower_elem_list(low, ast->tuple_lit.elems);
    TTNode *node = tt_new(low->tt_arena, TT_TUPLE_LIT, ast->type, ast->loc);
    node->tuple_lit.elems = elems;
    return node;
}

// Str interpolation lowering is in lower_str.c

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
static TTNode *collect_promoted_fields(Lowering *low, const ASTNode *ast, const Type *embed_type) {
    const char **sub_names = NULL;
    TTNode **sub_values = NULL;

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
    TTNode *sub = tt_new(low->tt_arena, TT_STRUCT_LIT, embed_type, ast->loc);
    sub->struct_lit.field_names = sub_names;
    sub->struct_lit.field_values = sub_values;
    return sub;
}

static TTNode *lower_struct_lit(Lowering *low, const ASTNode *ast) {
    const Type *struct_type = ast->type;
    if (struct_type == NULL || struct_type->kind != TYPE_STRUCT) {
        return tt_new(low->tt_arena, TT_UNIT_LIT, &TYPE_ERR_INST, ast->loc);
    }

    const char **field_names = NULL;
    TTNode **field_values = NULL;

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
                TTNode *sub = collect_promoted_fields(low, ast, sf->type);
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

    TTNode *node = tt_new(low->tt_arena, TT_STRUCT_LIT, struct_type, ast->loc);
    node->struct_lit.field_names = field_names;
    node->struct_lit.field_values = field_values;
    return node;
}

TTNode *lower_expr(Lowering *low, const ASTNode *ast) {
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
        TTNode *operand = lower_expr(low, ast->address_of.operand);
        // &StructLit{} → heap alloc
        if (ast->address_of.operand->kind == NODE_STRUCT_LIT) {
            TTNode *node = tt_new(low->tt_arena, TT_HEAP_ALLOC, ast->type, ast->loc);
            node->heap_alloc.operand = operand;
            return node;
        }
        // &var → native address-of
        TTNode *node = tt_new(low->tt_arena, TT_ADDRESS_OF, ast->type, ast->loc);
        node->address_of.operand = operand;
        return node;
    }
    case NODE_DEREF: {
        TTNode *operand = lower_expr(low, ast->deref.operand);
        TTNode *node = tt_new(low->tt_arena, TT_DEREF, ast->type, ast->loc);
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
    default:
        break;
    }
    return tt_new(low->tt_arena, TT_UNIT_LIT, &TYPE_ERR_INST, ast->loc);
}
