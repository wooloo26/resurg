#include "sema.h"

// ------------------------------------------------------------------------
// Private struct definitions (opaque in sema.h)
// ------------------------------------------------------------------------
struct Symbol {
    const char *name;
    const Type *type;
    bool is_pub;
    bool is_fn;
    struct Symbol *next; // intrusive linked list
};

struct Scope {
    Symbol *symbols;         // may be NULL
    struct Scope *parent;    // may be NULL (global scope)
    bool is_loop;            // allows break/continue
    const char *module_name; // set on module scope
};

struct Sema {
    Arena *arena;
    Scope *current_scope;
    int32_t error_count;
};

// ------------------------------------------------------------------------
// Scope
// ------------------------------------------------------------------------
static Scope *scope_push(Sema *s, bool is_loop) {
    Scope *sc = arena_alloc(s->arena, sizeof(Scope));
    sc->symbols = NULL;
    sc->parent = s->current_scope;
    sc->is_loop = is_loop;
    sc->module_name = s->current_scope != NULL ? s->current_scope->module_name : NULL;
    s->current_scope = sc;
    return sc;
}

static void scope_pop(Sema *s) {
    s->current_scope = s->current_scope->parent;
}

static void scope_define(Sema *s, const char *name, const Type *type, bool is_pub, bool is_fn) {
    Symbol *sym = arena_alloc(s->arena, sizeof(Symbol));
    sym->name = name;
    sym->type = type;
    sym->is_pub = is_pub;
    sym->is_fn = is_fn;
    sym->next = s->current_scope->symbols;
    s->current_scope->symbols = sym;
}

static Symbol *scope_lookup_current(const Sema *s, const char *name) {
    for (Symbol *sym = s->current_scope->symbols; sym != NULL; sym = sym->next) {
        if (strcmp(sym->name, name) == 0) {
            return sym;
        }
    }
    return NULL;
}

static Symbol *scope_lookup(const Sema *s, const char *name) {
    for (Scope *sc = s->current_scope; sc != NULL; sc = sc->parent) {
        for (Symbol *sym = sc->symbols; sym != NULL; sym = sym->next) {
            if (strcmp(sym->name, name) == 0) {
                return sym;
            }
        }
    }
    return NULL;
}

// ------------------------------------------------------------------------
// Helper: is scope inside a loop?
// ------------------------------------------------------------------------
static bool in_loop(const Sema *s) {
    for (Scope *sc = s->current_scope; sc != NULL; sc = sc->parent) {
        if (sc->is_loop) {
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------------
// Helper: resolve ASTType to Type*
// ------------------------------------------------------------------------
static const Type *resolve_ast_type(Sema *s, const ASTType *at) {
    if (at == NULL || at->kind == AST_TYPE_INFERRED) {
        return NULL;
    }
    const Type *t = type_from_name(at->name);
    if (t == NULL) {
        rg_error(at->loc, "unknown type '%s'", at->name);
        s->error_count++;
        return &TYPE_ERROR_INST;
    }
    return t;
}

// ------------------------------------------------------------------------
// Function signature storage (for forward references)
// ------------------------------------------------------------------------
typedef struct FnSig {
    const char *name;
    const Type *return_type;
    const Type **param_types; /* buf */
    int32_t param_count;
    bool is_pub;
} FnSig;

static FnSig *g_fn_sigs = NULL; /* buf */

static FnSig *find_fn_sig(const char *name) {
    for (int32_t i = 0; i < BUF_LEN(g_fn_sigs); i++) {
        if (strcmp(g_fn_sigs[i].name, name) == 0) {
            return &g_fn_sigs[i];
        }
    }
    return NULL;
}

// ------------------------------------------------------------------------
// Type checking — recursive AST walk
// ------------------------------------------------------------------------
static const Type *check_node(Sema *s, ASTNode *node);

static const Type *check_literal(Sema *s, ASTNode *node) {
    (void)s;
    switch (node->literal.kind) {
    case LIT_BOOL:
        return &TYPE_BOOL_INST;
    case LIT_I32:
        return &TYPE_I32_INST;
    case LIT_U32:
        return &TYPE_U32_INST;
    case LIT_F64:
        return &TYPE_F64_INST;
    case LIT_STR:
        return &TYPE_STR_INST;
    case LIT_UNIT:
        return &TYPE_UNIT_INST;
    }
    return &TYPE_ERROR_INST;
}

static const Type *check_ident(Sema *s, ASTNode *node) {
    Symbol *sym = scope_lookup(s, node->ident.name);
    if (sym == NULL) {
        rg_error(node->loc, "undefined variable '%s'", node->ident.name);
        s->error_count++;
        return &TYPE_ERROR_INST;
    }
    return sym->type;
}

static const Type *check_unary(Sema *s, ASTNode *node) {
    const Type *operand = check_node(s, node->unary.operand);
    if (node->unary.op == TOK_BANG) {
        return &TYPE_BOOL_INST;
    }
    if (node->unary.op == TOK_MINUS) {
        return operand;
    }
    return operand;
}

// Promote an integer literal node to match a target numeric type
static const Type *promote_literal(ASTNode *lit, const Type *target) {
    if (lit == NULL || lit->kind != NODE_LITERAL) {
        return NULL;
    }
    if (lit->literal.kind != LIT_I32 && lit->literal.kind != LIT_U32) {
        return NULL;
    }
    if (target->kind == TYPE_U32) {
        lit->literal.kind = LIT_U32;
    } else if (target->kind == TYPE_F64) {
        lit->literal.kind = LIT_F64;
        lit->literal.f64_val = (double)lit->literal.int_val;
    } else {
        return NULL;
    }
    lit->type = target;
    return target;
}

static const Type *check_binary(Sema *s, ASTNode *node) {
    const Type *left = check_node(s, node->binary.left);
    const Type *right = check_node(s, node->binary.right);

    // Promote integer literal to match the other side's numeric type
    if (left != NULL && right != NULL) {
        if (type_is_numeric(left) && type_is_numeric(right) && !type_eq(left, right)) {
            const Type *p;
            p = promote_literal(node->binary.left, right);
            if (p != NULL) {
                left = p;
            }
            p = promote_literal(node->binary.right, left);
            if (p != NULL) {
                right = p;
            }
        }
    }

    switch (node->binary.op) {
    // Comparison and logical operators return bool
    case TOK_EQ_EQ:
    case TOK_BANG_EQ:
    case TOK_LT:
    case TOK_LT_EQ:
    case TOK_GT:
    case TOK_GT_EQ:
    case TOK_AMP_AMP:
    case TOK_PIPE_PIPE:
        return &TYPE_BOOL_INST;

    // Arithmetic returns the operand type
    default:
        return left != NULL ? left : right;
    }
}

static const Type *check_call(Sema *s, ASTNode *node) {
    // Check callee
    const char *fn_name = NULL;
    if (node->call.callee->kind == NODE_IDENT) {
        fn_name = node->call.callee->ident.name;
    } else if (node->call.callee->kind == NODE_MEMBER) {
        fn_name = node->call.callee->member.member;
    }

    // Check arguments
    for (int32_t i = 0; i < BUF_LEN(node->call.args); i++) {
        check_node(s, node->call.args[i]);
    }

    // Look up function return type
    if (fn_name != NULL) {
        Symbol *sym = scope_lookup(s, fn_name);
        if (sym != NULL && sym->is_fn) {
            return sym->type;
        }
        // Try g_fn_sigs
        FnSig *sig = find_fn_sig(fn_name);
        if (sig != NULL) {
            return sig->return_type;
        }
        if (sym == NULL) {
            rg_error(node->loc, "undefined function '%s'", fn_name);
            s->error_count++;
        }
    }
    return &TYPE_ERROR_INST;
}

static const Type *check_if(Sema *s, ASTNode *node) {
    check_node(s, node->if_expr.cond);
    const Type *then_t = check_node(s, node->if_expr.then_body);
    const Type *else_t = NULL;
    if (node->if_expr.else_body != NULL) {
        else_t = check_node(s, node->if_expr.else_body);
    }

    // If both branches present and non-unit, return their common type
    if (else_t != NULL && then_t != NULL && then_t->kind != TYPE_UNIT) {
        return then_t;
    }
    if (else_t != NULL && else_t->kind != TYPE_UNIT) {
        return else_t;
    }
    if (then_t != NULL) {
        return then_t;
    }
    return &TYPE_UNIT_INST;
}

static const Type *check_block(Sema *s, ASTNode *node) {
    scope_push(s, false);
    for (int32_t i = 0; i < BUF_LEN(node->block.stmts); i++) {
        check_node(s, node->block.stmts[i]);
    }
    const Type *result_type = &TYPE_UNIT_INST;
    if (node->block.result != NULL) {
        result_type = check_node(s, node->block.result);
    }
    scope_pop(s);
    return result_type;
}

static const Type *check_var_decl(Sema *s, ASTNode *node) {
    const Type *init_type = NULL;
    if (node->var_decl.init != NULL) {
        init_type = check_node(s, node->var_decl.init);
    }

    const Type *declared = resolve_ast_type(s, &node->var_decl.type);

    // Determine final type
    const Type *var_type;
    if (declared != NULL) {
        var_type = declared;
        // If init is a literal, retype it to match declared type
        if (init_type != NULL && node->var_decl.init != NULL && node->var_decl.init->kind == NODE_LITERAL) {
            if (declared->kind == TYPE_U32 && node->var_decl.init->literal.kind == LIT_I32) {
                node->var_decl.init->literal.kind = LIT_U32;
                node->var_decl.init->type = declared;
            }
        }
    } else if (init_type != NULL) {
        var_type = init_type;
    } else {
        rg_error(node->loc, "cannot infer type for '%s'", node->var_decl.name);
        s->error_count++;
        var_type = &TYPE_ERROR_INST;
    }

    if (scope_lookup_current(s, node->var_decl.name) != NULL) {
        rg_error(node->loc, "redefinition of '%s' in the same scope", node->var_decl.name);
        s->error_count++;
    } else if (scope_lookup(s, node->var_decl.name) != NULL) {
        rg_error(node->loc, "variable '%s' shadows an existing binding", node->var_decl.name);
        s->error_count++;
    }

    scope_define(s, node->var_decl.name, var_type, false, false);
    return var_type;
}

static void check_fn_body(Sema *s, ASTNode *fn_node) {
    scope_push(s, false);

    // Register parameters
    for (int32_t i = 0; i < BUF_LEN(fn_node->fn_decl.params); i++) {
        ASTNode *param = fn_node->fn_decl.params[i];
        const Type *pt = resolve_ast_type(s, &param->param.type);
        if (pt == NULL) {
            pt = &TYPE_ERROR_INST;
        }
        param->type = pt;
        scope_define(s, param->param.name, pt, false, false);
    }

    // Check body
    if (fn_node->fn_decl.body != NULL) {
        const Type *body_type = check_node(s, fn_node->fn_decl.body);

        // If return type not declared, infer from body
        const Type *ret = resolve_ast_type(s, &fn_node->fn_decl.return_type);
        if (ret == NULL) {
            ret = body_type != NULL ? body_type : &TYPE_UNIT_INST;
        }
        fn_node->type = ret;

        // Update the function's symbol type to the resolved return type
        Symbol *sym = scope_lookup(s, fn_node->fn_decl.name);
        if (sym != NULL) {
            sym->type = ret;
        }

        // Update g_fn_sigs
        FnSig *sig = find_fn_sig(fn_node->fn_decl.name);
        if (sig != NULL && sig->return_type->kind == TYPE_UNIT) {
            sig->return_type = ret;
        }
    }

    scope_pop(s);
}

static const Type *check_assign(Sema *s, ASTNode *node) {
    check_node(s, node->assign.target);
    check_node(s, node->assign.value);
    return &TYPE_UNIT_INST;
}

static const Type *check_compound_assign(Sema *s, ASTNode *node) {
    check_node(s, node->compound_assign.target);
    check_node(s, node->compound_assign.value);
    return &TYPE_UNIT_INST;
}

static const Type *check_str_interp(Sema *s, ASTNode *node) {
    for (int32_t i = 0; i < BUF_LEN(node->str_interp.parts); i++) {
        check_node(s, node->str_interp.parts[i]);
    }
    return &TYPE_STR_INST;
}

static const Type *check_node(Sema *s, ASTNode *node) {
    if (node == NULL) {
        return &TYPE_UNIT_INST;
    }
    const Type *result = &TYPE_UNIT_INST;

    switch (node->kind) {
    case NODE_FILE:
        for (int32_t i = 0; i < BUF_LEN(node->file.decls); i++) {
            check_node(s, node->file.decls[i]);
        }
        break;

    case NODE_MODULE:
        s->current_scope->module_name = node->module.name;
        break;

    case NODE_FN_DECL:
        check_fn_body(s, node);
        result = node->type; // preserve type set by check_fn_body
        break;

    case NODE_VAR_DECL:
        result = check_var_decl(s, node);
        break;

    case NODE_PARAM:
        result = resolve_ast_type(s, &node->param.type);
        if (result == NULL) {
            result = &TYPE_ERROR_INST;
        }
        break;

    case NODE_EXPR_STMT:
        check_node(s, node->expr_stmt.expr);
        break;

    case NODE_ASSERT:
        check_node(s, node->assert_stmt.cond);
        if (node->assert_stmt.message != NULL) {
            check_node(s, node->assert_stmt.message);
        }
        break;

    case NODE_BREAK:
    case NODE_CONTINUE:
        if (!in_loop(s)) {
            rg_error(node->loc, "'%s' outside of loop", node->kind == NODE_BREAK ? "break" : "continue");
            s->error_count++;
        }
        break;

    case NODE_LITERAL:
        result = check_literal(s, node);
        break;

    case NODE_IDENT:
        result = check_ident(s, node);
        break;

    case NODE_UNARY:
        result = check_unary(s, node);
        break;

    case NODE_BINARY:
        result = check_binary(s, node);
        break;

    case NODE_ASSIGN:
        result = check_assign(s, node);
        break;

    case NODE_COMPOUND_ASSIGN:
        result = check_compound_assign(s, node);
        break;

    case NODE_CALL:
        result = check_call(s, node);
        break;

    case NODE_MEMBER:
        check_node(s, node->member.object);
        result = &TYPE_ERROR_INST;
        break;

    case NODE_IF:
        result = check_if(s, node);
        break;

    case NODE_LOOP:
        scope_push(s, true);
        check_node(s, node->loop.body);
        scope_pop(s);
        break;

    case NODE_FOR: {
        check_node(s, node->for_loop.start);
        check_node(s, node->for_loop.end);
        scope_push(s, true);
        scope_define(s, node->for_loop.var_name, &TYPE_I32_INST, false, false);
        check_node(s, node->for_loop.body);
        scope_pop(s);
        break;
    }

    case NODE_BLOCK:
        result = check_block(s, node);
        break;

    case NODE_STR_INTERP:
        result = check_str_interp(s, node);
        break;
    }

    node->type = result;
    return result;
}

// ------------------------------------------------------------------------
// Semantic analysis — public API
// ------------------------------------------------------------------------
Sema *sema_create(Arena *arena) {
    Sema *s = malloc(sizeof(*s));
    if (s == NULL) {
        rg_fatal("out of memory");
    }
    s->arena = arena;
    s->current_scope = NULL;
    s->error_count = 0;
    return s;
}

void sema_destroy(Sema *s) {
    free(s);
}

bool sema_check(Sema *s, ASTNode *file) {
    // Reset g_fn_sigs for each compilation
    if (g_fn_sigs != NULL) {
        BUF_FREE(g_fn_sigs);
        g_fn_sigs = NULL;
    }

    scope_push(s, false); // global scope

    // First pass: register all function signatures (forward declarations)
    for (int32_t i = 0; i < BUF_LEN(file->file.decls); i++) {
        ASTNode *decl = file->file.decls[i];
        if (decl->kind == NODE_FN_DECL) {
            // Resolve return type (may be inferred — default to unit)
            const Type *ret = &TYPE_UNIT_INST;
            if (decl->fn_decl.return_type.kind == AST_TYPE_NAME) {
                ret = type_from_name(decl->fn_decl.return_type.name);
                if (ret == NULL) {
                    ret = &TYPE_UNIT_INST;
                }
            }

            // Build param types
            FnSig sig;
            sig.name = decl->fn_decl.name;
            sig.return_type = ret;
            sig.param_types = NULL;
            sig.param_count = BUF_LEN(decl->fn_decl.params);
            sig.is_pub = decl->fn_decl.is_pub;
            for (int32_t j = 0; j < sig.param_count; j++) {
                ASTNode *param = decl->fn_decl.params[j];
                const Type *pt = type_from_name(param->param.type.name);
                if (pt == NULL) {
                    pt = &TYPE_ERROR_INST;
                }
                BUF_PUSH(sig.param_types, pt);
            }
            BUF_PUSH(g_fn_sigs, sig);

            // Register in scope
            scope_define(s, decl->fn_decl.name, ret, decl->fn_decl.is_pub, true);
        }
    }

    // Second pass: type-check everything
    check_node(s, file);

    scope_pop(s);
    return s->error_count == 0;
}
