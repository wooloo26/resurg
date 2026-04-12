#include "lsp_semtokens.h"

#include <string.h>

#include "core/common.h"
#include "repr/ast.h"

/**
 * @file lsp_semtokens.c
 * @brief Semantic token generation for LSP — walks AST and classifies tokens.
 */

// ── Internal types ─────────────────────────────────────────────────────

/** Context carried through the semantic token AST walk. */
typedef struct {
    SemToken *tokens;      /* buf */
    const char *file_path; // filter: only emit tokens from this file
    const char **params;   /* buf - current fn param names */
    const char *recv_name; // current fn receiver, or NULL
} SemWalkCtx;

// ── Forward declarations ───────────────────────────────────────────────

static void sem_walk(SemWalkCtx *ctx, const ASTNode *node);

// ── Helpers ────────────────────────────────────────────────────────────

static void sem_emit(SemWalkCtx *ctx, SrcLoc loc, int32_t len, int32_t type, int32_t mods) {
    if (loc.file != NULL && ctx->file_path != NULL && strcmp(loc.file, ctx->file_path) != 0) {
        return;
    }
    if (loc.line <= 0 || loc.column <= 0 || len <= 0) {
        return;
    }
    SemToken t = {
        .line = loc.line - 1,
        .start_char = loc.column - 1,
        .length = len,
        .token_type = type,
        .modifiers = mods,
    };
    BUF_PUSH(ctx->tokens, t);
}

/** Check if name matches a parameter or receiver in current scope. */
static bool sem_emit_id(SemWalkCtx *ctx, const ASTNode *node) {
    const char *name = node->id.name;
    int32_t len = (int32_t)strlen(name);
    if (ctx->recv_name != NULL && strcmp(name, ctx->recv_name) == 0) {
        sem_emit(ctx, node->loc, len, SEM_TYPE_PARAMETER, SEM_MOD_READONLY);
        return true;
    }
    for (int32_t i = 0; i < BUF_LEN(ctx->params); i++) {
        if (strcmp(name, ctx->params[i]) == 0) {
            sem_emit(ctx, node->loc, len, SEM_TYPE_PARAMETER, 0);
            return true;
        }
    }
    return false;
}

/** Walk children of an array of ASTNode pointers. */
static void sem_walk_arr(SemWalkCtx *ctx, ASTNode **arr) {
    for (int32_t i = 0; i < BUF_LEN(arr); i++) {
        sem_walk(ctx, arr[i]);
    }
}

/** Walk a function declaration: set up param scope, walk body. */
static void sem_walk_fn(SemWalkCtx *ctx, const ASTNode *node) {
    const char **prev_params = ctx->params;
    const char *prev_recv = ctx->recv_name;
    ctx->params = NULL;
    ctx->recv_name = node->fn_decl.recv_name;
    for (int32_t i = 0; i < BUF_LEN(node->fn_decl.params); i++) {
        const ASTNode *p = node->fn_decl.params[i];
        if (p->kind == NODE_PARAM) {
            BUF_PUSH(ctx->params, p->param.name);
        }
    }
    sem_walk(ctx, node->fn_decl.body);
    BUF_FREE(ctx->params);
    ctx->params = prev_params;
    ctx->recv_name = prev_recv;
}

// ── Main walker ────────────────────────────────────────────────────────

/** Recursive AST walk for semantic token collection. */
static void sem_walk(SemWalkCtx *ctx, const ASTNode *node) {
    if (node == NULL) {
        return;
    }
    switch (node->kind) {
    case NODE_FILE:
        sem_walk_arr(ctx, node->file.decls);
        break;
    case NODE_FN_DECL:
        sem_walk_fn(ctx, node);
        break;
    case NODE_CLOSURE: {
        const char **prev_params = ctx->params;
        const char *prev_recv = ctx->recv_name;
        ctx->params = NULL;
        ctx->recv_name = NULL;
        for (int32_t i = 0; i < BUF_LEN(node->closure.params); i++) {
            const ASTNode *p = node->closure.params[i];
            if (p->kind == NODE_PARAM) {
                BUF_PUSH(ctx->params, p->param.name);
            }
        }
        sem_walk(ctx, node->closure.body);
        BUF_FREE(ctx->params);
        ctx->params = prev_params;
        ctx->recv_name = prev_recv;
        break;
    }
    case NODE_ID:
        sem_emit_id(ctx, node);
        break;
    case NODE_BLOCK:
        sem_walk_arr(ctx, node->block.stmts);
        sem_walk(ctx, node->block.result);
        break;
    case NODE_EXPR_STMT:
        sem_walk(ctx, node->expr_stmt.expr);
        break;
    case NODE_CALL:
        sem_walk(ctx, node->call.callee);
        sem_walk_arr(ctx, node->call.args);
        break;
    case NODE_BINARY:
        sem_walk(ctx, node->binary.left);
        sem_walk(ctx, node->binary.right);
        break;
    case NODE_UNARY:
        sem_walk(ctx, node->unary.operand);
        break;
    case NODE_ASSIGN:
        sem_walk(ctx, node->assign.target);
        sem_walk(ctx, node->assign.value);
        break;
    case NODE_COMPOUND_ASSIGN:
        sem_walk(ctx, node->compound_assign.target);
        sem_walk(ctx, node->compound_assign.value);
        break;
    case NODE_MEMBER:
        sem_walk(ctx, node->member.object);
        break;
    case NODE_IDX:
        sem_walk(ctx, node->idx_access.object);
        sem_walk(ctx, node->idx_access.idx);
        break;
    case NODE_IF:
        sem_walk(ctx, node->if_expr.cond);
        sem_walk(ctx, node->if_expr.then_body);
        sem_walk(ctx, node->if_expr.else_body);
        sem_walk(ctx, node->if_expr.pattern_init);
        break;
    case NODE_LOOP:
        sem_walk(ctx, node->loop.body);
        break;
    case NODE_WHILE:
        sem_walk(ctx, node->while_loop.cond);
        sem_walk(ctx, node->while_loop.body);
        sem_walk(ctx, node->while_loop.pattern_init);
        break;
    case NODE_FOR:
        sem_walk(ctx, node->for_loop.start);
        sem_walk(ctx, node->for_loop.end);
        sem_walk(ctx, node->for_loop.iterable);
        sem_walk(ctx, node->for_loop.body);
        break;
    case NODE_RETURN:
        sem_walk(ctx, node->return_stmt.value);
        break;
    case NODE_VAR_DECL:
        sem_walk(ctx, node->var_decl.init);
        break;
    case NODE_STR_INTERPOLATION:
        sem_walk_arr(ctx, node->str_interpolation.parts);
        break;
    case NODE_ARRAY_LIT:
        sem_walk_arr(ctx, node->array_lit.elems);
        break;
    case NODE_SLICE_LIT:
        sem_walk_arr(ctx, node->slice_lit.elems);
        break;
    case NODE_SLICE_EXPR:
        sem_walk(ctx, node->slice_expr.object);
        sem_walk(ctx, node->slice_expr.start);
        sem_walk(ctx, node->slice_expr.end);
        break;
    case NODE_TUPLE_LIT:
        sem_walk_arr(ctx, node->tuple_lit.elems);
        break;
    case NODE_TYPE_CONVERSION:
        sem_walk(ctx, node->type_conversion.operand);
        break;
    case NODE_STRUCT_DECL:
        sem_walk_arr(ctx, node->struct_decl->methods);
        break;
    case NODE_STRUCT_LIT:
        sem_walk_arr(ctx, node->struct_lit.field_values);
        break;
    case NODE_ENUM_DECL:
        sem_walk_arr(ctx, node->enum_decl->methods);
        break;
    case NODE_MATCH:
        sem_walk(ctx, node->match_expr.operand);
        for (int32_t i = 0; i < BUF_LEN(node->match_expr.arms); i++) {
            sem_walk(ctx, node->match_expr.arms[i].body);
        }
        break;
    case NODE_ENUM_INIT:
        sem_walk_arr(ctx, node->enum_init.args);
        sem_walk_arr(ctx, node->enum_init.field_values);
        break;
    case NODE_PACT_DECL:
        sem_walk_arr(ctx, node->pact_decl->methods);
        break;
    case NODE_EXT_DECL:
        sem_walk_arr(ctx, node->ext_decl->methods);
        break;
    case NODE_ADDRESS_OF:
        sem_walk(ctx, node->address_of.operand);
        break;
    case NODE_DEREF:
        sem_walk(ctx, node->deref.operand);
        break;
    case NODE_OPTIONAL_CHAIN:
        sem_walk(ctx, node->optional_chain.object);
        break;
    case NODE_TRY:
        sem_walk(ctx, node->try_expr.operand);
        break;
    case NODE_DEFER:
        sem_walk(ctx, node->defer_stmt.body);
        break;
    case NODE_BREAK:
        sem_walk(ctx, node->break_stmt.value);
        break;
    case NODE_STRUCT_DESTRUCTURE:
        sem_walk(ctx, node->struct_destructure.value);
        break;
    case NODE_TUPLE_DESTRUCTURE:
        sem_walk(ctx, node->tuple_destructure.value);
        break;
    default:
        break;
    }
}

// ── Public API ─────────────────────────────────────────────────────────

SemToken *lsp_build_semantic_tokens(const ASTNode *file_node, const char *file_path) {
    SemWalkCtx ctx = {0};
    ctx.file_path = file_path;
    sem_walk(&ctx, file_node);
    return ctx.tokens;
}

int lsp_sem_token_cmp(const void *a, const void *b) {
    const SemToken *ta = (const SemToken *)a;
    const SemToken *tb = (const SemToken *)b;
    if (ta->line != tb->line) {
        return ta->line - tb->line;
    }
    return ta->start_char - tb->start_char;
}
