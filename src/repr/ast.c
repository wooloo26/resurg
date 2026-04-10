#include "repr/ast.h"

// ── Construction ──────────────────────────────────────────────────────

ASTNode *ast_new(Arena *arena, NodeKind kind, SrcLoc loc) {
    ASTNode *node = arena_alloc_zero(arena, sizeof(ASTNode));
    node->kind = kind;
    node->loc = loc;
    node->type = NULL;
    // Allocate heap-indirect data for large decl variants.
    switch (kind) {
    case NODE_STRUCT_DECL:
        node->struct_decl = arena_alloc_zero(arena, sizeof(StructDeclData));
        break;
    case NODE_ENUM_DECL:
        node->enum_decl = arena_alloc_zero(arena, sizeof(EnumDeclData));
        break;
    case NODE_PACT_DECL:
        node->pact_decl = arena_alloc_zero(arena, sizeof(PactDeclData));
        break;
    case NODE_EXT_DECL:
        node->ext_decl = arena_alloc_zero(arena, sizeof(ExtDeclData));
        break;
    default:
        break;
    }
    return node;
}

// ── Deep clone helpers ────────────────────────────────────────────

/** Clone a stretchy buf of ASTNode ptrs into a fresh buf. */
static ASTNode **clone_node_buf(Arena *arena, ASTNode **src_buf) {
    ASTNode **dst_buf = NULL;
    for (int32_t i = 0; i < BUF_LEN(src_buf); i++) {
        BUF_PUSH(dst_buf, ast_clone(arena, src_buf[i]));
    }
    return dst_buf;
}

/** Clone a stretchy buf of const-char ptrs (shallow copy of each str). */
static const char **clone_str_buf(const char **src_buf) {
    const char **dst_buf = NULL;
    for (int32_t i = 0; i < BUF_LEN(src_buf); i++) {
        BUF_PUSH(dst_buf, src_buf[i]);
    }
    return dst_buf;
}

/** Clone a stretchy buf of ASTType values (shallow copy, clear resolved cache). */
static ASTType *clone_ast_type_buf(ASTType *src_buf) {
    ASTType *dst_buf = NULL;
    for (int32_t i = 0; i < BUF_LEN(src_buf); i++) {
        ASTType cloned = src_buf[i];
        cloned.resolved = NULL;
        BUF_PUSH(dst_buf, cloned);
    }
    return dst_buf;
}

/** Clone a stretchy buf of bools. */
static bool *clone_bool_buf(bool *src_buf) {
    bool *dst_buf = NULL;
    for (int32_t i = 0; i < BUF_LEN(src_buf); i++) {
        BUF_PUSH(dst_buf, src_buf[i]);
    }
    return dst_buf;
}

// ── Deep clone ────────────────────────────────────────────────────

/** Handler signature: copy kind-specific fields from @p src into @p dst. */
typedef void (*ASTCloneHandler)(Arena *arena, const ASTNode *src, ASTNode *dst);

// ── Leaf nodes ────────────────────────────────────────────────────

static void clone_lit(Arena *arena, const ASTNode *src, ASTNode *dst) {
    (void)arena;
    dst->lit = src->lit;
}

static void clone_id(Arena *arena, const ASTNode *src, ASTNode *dst) {
    (void)arena;
    dst->id = src->id;
}

static void clone_continue(Arena *arena, const ASTNode *src, ASTNode *dst) {
    (void)arena;
    (void)src;
    (void)dst;
}

static void clone_param(Arena *arena, const ASTNode *src, ASTNode *dst) {
    (void)arena;
    dst->param = src->param;
    dst->param.type.resolved = NULL;
}

// ── Single-child nodes ───────────────────────────────────────────

static void clone_unary(Arena *arena, const ASTNode *src, ASTNode *dst) {
    dst->unary.op = src->unary.op;
    dst->unary.operand = ast_clone(arena, src->unary.operand);
}

static void clone_expr_stmt(Arena *arena, const ASTNode *src, ASTNode *dst) {
    dst->expr_stmt.expr = ast_clone(arena, src->expr_stmt.expr);
}

static void clone_return(Arena *arena, const ASTNode *src, ASTNode *dst) {
    dst->return_stmt.value = ast_clone(arena, src->return_stmt.value);
}

static void clone_break(Arena *arena, const ASTNode *src, ASTNode *dst) {
    dst->break_stmt.value = ast_clone(arena, src->break_stmt.value);
}

static void clone_loop(Arena *arena, const ASTNode *src, ASTNode *dst) {
    dst->loop.body = ast_clone(arena, src->loop.body);
}

static void clone_defer(Arena *arena, const ASTNode *src, ASTNode *dst) {
    dst->defer_stmt.body = ast_clone(arena, src->defer_stmt.body);
}

static void clone_address_of(Arena *arena, const ASTNode *src, ASTNode *dst) {
    dst->address_of.operand = ast_clone(arena, src->address_of.operand);
}

static void clone_deref(Arena *arena, const ASTNode *src, ASTNode *dst) {
    dst->deref.operand = ast_clone(arena, src->deref.operand);
}

static void clone_try(Arena *arena, const ASTNode *src, ASTNode *dst) {
    dst->try_expr.operand = ast_clone(arena, src->try_expr.operand);
}

// ── Two-child nodes ─────────────────────────────────────────────

static void clone_binary(Arena *arena, const ASTNode *src, ASTNode *dst) {
    dst->binary.op = src->binary.op;
    dst->binary.left = ast_clone(arena, src->binary.left);
    dst->binary.right = ast_clone(arena, src->binary.right);
}

static void clone_assign(Arena *arena, const ASTNode *src, ASTNode *dst) {
    dst->assign.target = ast_clone(arena, src->assign.target);
    dst->assign.value = ast_clone(arena, src->assign.value);
}

static void clone_compound_assign(Arena *arena, const ASTNode *src, ASTNode *dst) {
    dst->compound_assign.op = src->compound_assign.op;
    dst->compound_assign.target = ast_clone(arena, src->compound_assign.target);
    dst->compound_assign.value = ast_clone(arena, src->compound_assign.value);
}

static void clone_idx(Arena *arena, const ASTNode *src, ASTNode *dst) {
    dst->idx_access.object = ast_clone(arena, src->idx_access.object);
    dst->idx_access.idx = ast_clone(arena, src->idx_access.idx);
}

static void clone_member(Arena *arena, const ASTNode *src, ASTNode *dst) {
    dst->member.object = ast_clone(arena, src->member.object);
    dst->member.member = src->member.member;
}

static void clone_optional_chain(Arena *arena, const ASTNode *src, ASTNode *dst) {
    dst->optional_chain.object = ast_clone(arena, src->optional_chain.object);
    dst->optional_chain.member = src->optional_chain.member;
}

static void clone_while(Arena *arena, const ASTNode *src, ASTNode *dst) {
    dst->while_loop.cond = ast_clone(arena, src->while_loop.cond);
    dst->while_loop.body = ast_clone(arena, src->while_loop.body);
    dst->while_loop.pattern = src->while_loop.pattern;
    dst->while_loop.pattern_init = ast_clone(arena, src->while_loop.pattern_init);
}

static void clone_type_conversion(Arena *arena, const ASTNode *src, ASTNode *dst) {
    dst->type_conversion.target_type = src->type_conversion.target_type;
    dst->type_conversion.target_type.resolved = NULL;
    dst->type_conversion.operand = ast_clone(arena, src->type_conversion.operand);
}

// ── Multi-child nodes ───────────────────────────────────────────

static void clone_block(Arena *arena, const ASTNode *src, ASTNode *dst) {
    dst->block.stmts = clone_node_buf(arena, src->block.stmts);
    dst->block.result = ast_clone(arena, src->block.result);
}

static void clone_if(Arena *arena, const ASTNode *src, ASTNode *dst) {
    dst->if_expr.cond = ast_clone(arena, src->if_expr.cond);
    dst->if_expr.then_body = ast_clone(arena, src->if_expr.then_body);
    dst->if_expr.else_body = ast_clone(arena, src->if_expr.else_body);
    dst->if_expr.pattern = src->if_expr.pattern;
    dst->if_expr.pattern_init = ast_clone(arena, src->if_expr.pattern_init);
}

static void clone_var_decl(Arena *arena, const ASTNode *src, ASTNode *dst) {
    dst->var_decl = src->var_decl;
    dst->var_decl.type.resolved = NULL;
    dst->var_decl.init = ast_clone(arena, src->var_decl.init);
}

static void clone_call(Arena *arena, const ASTNode *src, ASTNode *dst) {
    dst->call.callee = ast_clone(arena, src->call.callee);
    dst->call.args = clone_node_buf(arena, src->call.args);
    dst->call.arg_names = clone_str_buf(src->call.arg_names);
    dst->call.arg_is_mut = clone_bool_buf(src->call.arg_is_mut);
    dst->call.arg_is_spread = clone_bool_buf(src->call.arg_is_spread);
    dst->call.type_args = clone_ast_type_buf(src->call.type_args);
    dst->call.variadic_start = src->call.variadic_start;
    dst->call.variadic_type = src->call.variadic_type;
}

static void clone_for(Arena *arena, const ASTNode *src, ASTNode *dst) {
    dst->for_loop = src->for_loop;
    dst->for_loop.start = ast_clone(arena, src->for_loop.start);
    dst->for_loop.end = ast_clone(arena, src->for_loop.end);
    dst->for_loop.iterable = ast_clone(arena, src->for_loop.iterable);
    dst->for_loop.body = ast_clone(arena, src->for_loop.body);
}

static void clone_str_interpolation(Arena *arena, const ASTNode *src, ASTNode *dst) {
    dst->str_interpolation.parts = clone_node_buf(arena, src->str_interpolation.parts);
}

static void clone_array_lit(Arena *arena, const ASTNode *src, ASTNode *dst) {
    dst->array_lit = src->array_lit;
    dst->array_lit.elem_type.resolved = NULL;
    dst->array_lit.elems = clone_node_buf(arena, src->array_lit.elems);
}

static void clone_slice_lit(Arena *arena, const ASTNode *src, ASTNode *dst) {
    dst->slice_lit = src->slice_lit;
    dst->slice_lit.elem_type.resolved = NULL;
    dst->slice_lit.elems = clone_node_buf(arena, src->slice_lit.elems);
}

static void clone_slice_expr(Arena *arena, const ASTNode *src, ASTNode *dst) {
    dst->slice_expr.object = ast_clone(arena, src->slice_expr.object);
    dst->slice_expr.start = ast_clone(arena, src->slice_expr.start);
    dst->slice_expr.end = ast_clone(arena, src->slice_expr.end);
    dst->slice_expr.full_range = src->slice_expr.full_range;
}

static void clone_tuple_lit(Arena *arena, const ASTNode *src, ASTNode *dst) {
    dst->tuple_lit.elems = clone_node_buf(arena, src->tuple_lit.elems);
}

static void clone_struct_lit(Arena *arena, const ASTNode *src, ASTNode *dst) {
    dst->struct_lit.name = src->struct_lit.name;
    dst->struct_lit.field_names = clone_str_buf(src->struct_lit.field_names);
    dst->struct_lit.field_values = clone_node_buf(arena, src->struct_lit.field_values);
    dst->struct_lit.type_args = clone_ast_type_buf(src->struct_lit.type_args);
}

static void clone_struct_destructure(Arena *arena, const ASTNode *src, ASTNode *dst) {
    dst->struct_destructure.field_names = clone_str_buf(src->struct_destructure.field_names);
    dst->struct_destructure.aliases = clone_str_buf(src->struct_destructure.aliases);
    dst->struct_destructure.value = ast_clone(arena, src->struct_destructure.value);
}

static void clone_tuple_destructure(Arena *arena, const ASTNode *src, ASTNode *dst) {
    dst->tuple_destructure.names = clone_str_buf(src->tuple_destructure.names);
    dst->tuple_destructure.value = ast_clone(arena, src->tuple_destructure.value);
    dst->tuple_destructure.has_rest = src->tuple_destructure.has_rest;
    dst->tuple_destructure.rest_pos = src->tuple_destructure.rest_pos;
}

static void clone_closure(Arena *arena, const ASTNode *src, ASTNode *dst) {
    dst->closure.return_type = src->closure.return_type;
    dst->closure.return_type.resolved = NULL;
    dst->closure.params = clone_node_buf(arena, src->closure.params);
    dst->closure.body = ast_clone(arena, src->closure.body);
}

static void clone_match(Arena *arena, const ASTNode *src, ASTNode *dst) {
    dst->match_expr.operand = ast_clone(arena, src->match_expr.operand);
    dst->match_expr.arms = NULL;
    for (int32_t i = 0; i < BUF_LEN(src->match_expr.arms); i++) {
        ASTMatchArm arm = src->match_expr.arms[i];
        arm.body = ast_clone(arena, arm.body);
        arm.guard = ast_clone(arena, arm.guard);
        BUF_PUSH(dst->match_expr.arms, arm);
    }
}

static void clone_enum_init(Arena *arena, const ASTNode *src, ASTNode *dst) {
    dst->enum_init.enum_name = src->enum_init.enum_name;
    dst->enum_init.variant_name = src->enum_init.variant_name;
    dst->enum_init.args = clone_node_buf(arena, src->enum_init.args);
    dst->enum_init.field_names = clone_str_buf(src->enum_init.field_names);
    dst->enum_init.field_values = clone_node_buf(arena, src->enum_init.field_values);
    dst->enum_init.type_args = clone_ast_type_buf(src->enum_init.type_args);
}

// ── Decl nodes ──────────────────────────────────────────────────

static void clone_fn_decl(Arena *arena, const ASTNode *src, ASTNode *dst) {
    dst->fn_decl = src->fn_decl;
    dst->fn_decl.return_type.resolved = NULL;
    dst->fn_decl.params = clone_node_buf(arena, src->fn_decl.params);
    dst->fn_decl.body = ast_clone(arena, src->fn_decl.body);
}

static void clone_file(Arena *arena, const ASTNode *src, ASTNode *dst) {
    dst->file.decls = clone_node_buf(arena, src->file.decls);
}

static void clone_module(Arena *arena, const ASTNode *src, ASTNode *dst) {
    dst->module = src->module;
    dst->module.decls = clone_node_buf(arena, src->module.decls);
}

static void clone_type_alias(Arena *arena, const ASTNode *src, ASTNode *dst) {
    (void)arena;
    dst->type_alias = src->type_alias;
    dst->type_alias.alias_type.resolved = NULL;
}

static void clone_use_decl(Arena *arena, const ASTNode *src, ASTNode *dst) {
    (void)arena;
    dst->use_decl = src->use_decl;
    dst->use_decl.imported_names = clone_str_buf(src->use_decl.imported_names);
    dst->use_decl.aliases = clone_str_buf(src->use_decl.aliases);
}

// ── Dispatch table ──────────────────────────────────────────────

static const ASTCloneHandler clone_handlers[NODE_KIND_COUNT] = {
    [NODE_LIT] = clone_lit,
    [NODE_ID] = clone_id,
    [NODE_CONTINUE] = clone_continue,
    [NODE_PARAM] = clone_param,
    [NODE_UNARY] = clone_unary,
    [NODE_EXPR_STMT] = clone_expr_stmt,
    [NODE_RETURN] = clone_return,
    [NODE_BREAK] = clone_break,
    [NODE_LOOP] = clone_loop,
    [NODE_DEFER] = clone_defer,
    [NODE_ADDRESS_OF] = clone_address_of,
    [NODE_DEREF] = clone_deref,
    [NODE_TRY] = clone_try,
    [NODE_BINARY] = clone_binary,
    [NODE_ASSIGN] = clone_assign,
    [NODE_COMPOUND_ASSIGN] = clone_compound_assign,
    [NODE_IDX] = clone_idx,
    [NODE_MEMBER] = clone_member,
    [NODE_OPTIONAL_CHAIN] = clone_optional_chain,
    [NODE_WHILE] = clone_while,
    [NODE_TYPE_CONVERSION] = clone_type_conversion,
    [NODE_BLOCK] = clone_block,
    [NODE_IF] = clone_if,
    [NODE_VAR_DECL] = clone_var_decl,
    [NODE_CALL] = clone_call,
    [NODE_FOR] = clone_for,
    [NODE_STR_INTERPOLATION] = clone_str_interpolation,
    [NODE_ARRAY_LIT] = clone_array_lit,
    [NODE_SLICE_LIT] = clone_slice_lit,
    [NODE_SLICE_EXPR] = clone_slice_expr,
    [NODE_TUPLE_LIT] = clone_tuple_lit,
    [NODE_STRUCT_LIT] = clone_struct_lit,
    [NODE_STRUCT_DESTRUCTURE] = clone_struct_destructure,
    [NODE_TUPLE_DESTRUCTURE] = clone_tuple_destructure,
    [NODE_CLOSURE] = clone_closure,
    [NODE_MATCH] = clone_match,
    [NODE_ENUM_INIT] = clone_enum_init,
    [NODE_FN_DECL] = clone_fn_decl,
    [NODE_FILE] = clone_file,
    [NODE_MODULE] = clone_module,
    [NODE_TYPE_ALIAS] = clone_type_alias,
    [NODE_USE_DECL] = clone_use_decl,
    // NODE_STRUCT_DECL, NODE_ENUM_DECL, NODE_PACT_DECL, NODE_EXT_DECL:
    // left NULL — not cloned during monomorphization.
};

ASTNode *ast_clone(Arena *arena, ASTNode *src) {
    if (src == NULL) {
        return NULL;
    }
    ASTNode *dst = ast_new(arena, src->kind, src->loc);
    ASTCloneHandler handler = clone_handlers[src->kind];
    if (handler == NULL) {
        rsg_fatal("ast_clone: no handler for node kind %d", (int)src->kind);
    }
    handler(arena, src, dst);
    return dst;
}
