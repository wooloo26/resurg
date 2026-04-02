#include "codegen.h"

// ---------------------------------------------------------------------------
// VarEntry — private implementation (opaque in codegen.h)
// ---------------------------------------------------------------------------
struct VarEntry {
    const char *rg_name; // Resurg name
    const char *c_name;  // C name (may be mangled for shadowing)
};

struct CodeGen {
    FILE *out;               // output file handle
    Arena *arena;            // for temporary string building
    int32_t indent;          // current indentation level
    const char *module;      // current module name (for name mangling)
    const char *source_file; // escaped source file path for assert output
    int32_t tmp_counter;     // counter for temp variable names
    int32_t sb_counter;      // counter for string builder names
    VarEntry *vars;          /* buf */
    int32_t shadow_counter;  // counter for shadow name mangling
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void emit_indent(CodeGen *cg) {
    for (int32_t i = 0; i < cg->indent; i++) {
        fprintf(cg->out, "    ");
    }
}

static void emit(CodeGen *cg, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(cg->out, fmt, args);
    va_end(args);
}

static void emit_line(CodeGen *cg, const char *fmt, ...) {
    emit_indent(cg);
    va_list args;
    va_start(args, fmt);
    vfprintf(cg->out, fmt, args);
    va_end(args);
    fprintf(cg->out, "\n");
}

static const char *next_tmp(CodeGen *cg) {
    return arena_sprintf(cg->arena, "_rg_tmp_%d", cg->tmp_counter++);
}

static const char *next_sb(CodeGen *cg) {
    return arena_sprintf(cg->arena, "_rg_sb_%d", cg->sb_counter++);
}

// ---------------------------------------------------------------------------
// Variable name tracking (for shadowed variables)
// ---------------------------------------------------------------------------
static int32_t var_find(const CodeGen *cg, const char *name) {
    for (int32_t i = BUF_LEN(cg->vars) - 1; i >= 0; i--) {
        if (strcmp(cg->vars[i].rg_name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static const char *var_lookup(const CodeGen *cg, const char *name) {
    int32_t idx = var_find(cg, name);
    return (idx >= 0) ? cg->vars[idx].c_name : name;
}

static const char *var_define(CodeGen *cg, const char *name) {
    const char *c_name =
        (var_find(cg, name) >= 0) ? arena_sprintf(cg->arena, "%s__%d", name, cg->shadow_counter++) : name;
    VarEntry entry = {name, c_name};
    BUF_PUSH(cg->vars, entry);
    return c_name;
}

static void var_scope_reset(CodeGen *cg) {
    if (cg->vars != NULL) {
        BUF_FREE(cg->vars);
        cg->vars = NULL;
    }
    cg->shadow_counter = 0;
}

// ---------------------------------------------------------------------------
// Function name mangling (avoid C reserved words / stdlib conflicts)
// ---------------------------------------------------------------------------
static const char *mangle_fn_name(CodeGen *cg, const char *name) {
    if (strcmp(name, "main") == 0) {
        return "main";
    }
    return arena_sprintf(cg->arena, "rg_%s", name);
}

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static const char *emit_expr(CodeGen *cg, const ASTNode *node);
static void emit_stmt(CodeGen *cg, const ASTNode *node);
static void emit_block_stmts(CodeGen *cg, const ASTNode *block);

// ---------------------------------------------------------------------------
// Helpers for ternary optimization
// ---------------------------------------------------------------------------
// Return the simple result expression from a branch body, or NULL if the body
// contains statements that prevent ternary emission.
static const ASTNode *simple_branch_result(const ASTNode *body) {
    if (body == NULL) {
        return NULL;
    }
    if (body->kind == NODE_BLOCK) {
        if (BUF_LEN(body->block.stmts) != 0 || body->block.result == NULL) {
            return NULL;
        }
        return body->block.result;
    }
    // Direct expression (not a block)
    return body;
}

// Check whether emit_expr on this node is pure (returns a C expression string
// without emitting any lines to the output file).
static bool is_pure_expr(const ASTNode *node) {
    if (node == NULL) {
        return false;
    }
    switch (node->kind) {
    case NODE_LITERAL:
    case NODE_IDENT:
        return true;
    case NODE_UNARY:
        return is_pure_expr(node->unary.operand);
    case NODE_BINARY:
        return is_pure_expr(node->binary.left) && is_pure_expr(node->binary.right);
    default:
        return false;
    }
}

// Check whether an if-expression can be emitted as a C ternary.
static bool is_simple_ternary(const ASTNode *node) {
    if (node->kind != NODE_IF || node->if_expr.else_body == NULL) {
        return false;
    }
    const ASTNode *then_result = simple_branch_result(node->if_expr.then_body);
    if (then_result == NULL || !is_pure_expr(then_result)) {
        return false;
    }
    const ASTNode *else_body = node->if_expr.else_body;
    if (else_body->kind == NODE_IF) {
        return false; // else-if chains stay as statements
    }
    const ASTNode *else_result = simple_branch_result(else_body);
    return else_result != NULL && is_pure_expr(else_result);
}

// ---------------------------------------------------------------------------
// C operator string
// ---------------------------------------------------------------------------
static const char *c_binop(TokenKind op) {
    switch (op) {
    case TOK_PLUS:
        return "+";
    case TOK_MINUS:
        return "-";
    case TOK_STAR:
        return "*";
    case TOK_SLASH:
        return "/";
    case TOK_PERCENT:
        return "%";
    case TOK_EQ_EQ:
        return "==";
    case TOK_BANG_EQ:
        return "!=";
    case TOK_LT:
        return "<";
    case TOK_LT_EQ:
        return "<=";
    case TOK_GT:
        return ">";
    case TOK_GT_EQ:
        return ">=";
    case TOK_AMP_AMP:
        return "&&";
    case TOK_PIPE_PIPE:
        return "||";
    default:
        return "??";
    }
}

static const char *c_compound_op(TokenKind op) {
    switch (op) {
    case TOK_PLUS_EQ:
        return "+=";
    case TOK_MINUS_EQ:
        return "-=";
    case TOK_STAR_EQ:
        return "*=";
    case TOK_SLASH_EQ:
        return "/=";
    default:
        return "?=";
    }
}

// ---------------------------------------------------------------------------
// Escape a string for C string literal output
// ---------------------------------------------------------------------------
static const char *c_str_escape(const CodeGen *cg, const char *s) {
    if (s == NULL) {
        return "";
    }
    // Count extra space needed for escapes
    int32_t extra = 0;
    for (const char *p = s; *p != '\0'; p++) {
        switch (*p) {
        case '\\':
        case '"':
        case '\n':
        case '\r':
        case '\t':
            extra++;
            break;
        default:
            break;
        }
    }
    if (extra == 0) {
        return s;
    }
    int32_t len = (int32_t)strlen(s);
    char *buf = arena_alloc(cg->arena, len + extra + 1);
    int32_t j = 0;
    for (const char *p = s; *p != '\0'; p++) {
        switch (*p) {
        case '\\':
            buf[j++] = '\\';
            buf[j++] = '\\';
            break;
        case '"':
            buf[j++] = '\\';
            buf[j++] = '"';
            break;
        case '\n':
            buf[j++] = '\\';
            buf[j++] = 'n';
            break;
        case '\r':
            buf[j++] = '\\';
            buf[j++] = 'r';
            break;
        case '\t':
            buf[j++] = '\\';
            buf[j++] = 't';
            break;
        default:
            buf[j++] = *p;
            break;
        }
    }
    buf[j] = '\0';
    return buf;
}

// Escape a file path for embedding in C string (backslash -> forward slash)
static const char *c_escape_path(const CodeGen *cg, const char *path) {
    if (path == NULL) {
        return "";
    }
    int32_t len = (int32_t)strlen(path);
    char *buf = arena_alloc(cg->arena, len + 1);
    for (int32_t i = 0; i < len; i++) {
        buf[i] = (char)((path[i] == '\\') ? '/' : path[i]);
    }
    buf[len] = '\0';
    return buf;
}

// Format a double as a C literal string (ensures trailing .0 if needed)
static const char *fmt_f64(const CodeGen *cg, double v) {
    char buf[64];
    int32_t len = snprintf(buf, sizeof(buf), "%.17g", v);
    bool has_dot = false;
    for (int32_t i = 0; i < len; i++) {
        if (buf[i] == '.' || buf[i] == 'e' || buf[i] == 'E') {
            has_dot = true;
            break;
        }
    }
    if (!has_dot) {
        buf[len] = '.';
        buf[len + 1] = '0';
        buf[len + 2] = '\0';
    }
    return arena_strdup(cg->arena, buf);
}

// ---------------------------------------------------------------------------
// Expression emission — returns a C expression string
// ---------------------------------------------------------------------------
// Emit an if/else branch body, optionally assigning the result to a target
// variable.
static void emit_branch(CodeGen *cg, const ASTNode *body, const char *target) {
    if (body->kind == NODE_BLOCK) {
        if (target != NULL) {
            for (int32_t i = 0; i < BUF_LEN(body->block.stmts); i++) {
                emit_stmt(cg, body->block.stmts[i]);
            }
            if (body->block.result != NULL) {
                const char *v = emit_expr(cg, body->block.result);
                emit_line(cg, "%s = %s;", target, v);
            }
        } else {
            emit_block_stmts(cg, body);
            if (body->block.result != NULL) {
                emit_stmt(cg, body->block.result);
            }
        }
    } else {
        if (target != NULL) {
            const char *v = emit_expr(cg, body);
            emit_line(cg, "%s = %s;", target, v);
        } else {
            emit_stmt(cg, body);
        }
    }
}

// Emit an if-expression or if-statement.
// If target is non-NULL, assigns result to target (expression mode).
// If target is NULL, emits as statement (no result value).
static void emit_if(CodeGen *cg, const ASTNode *node, const char *target, bool is_else_if) {
    const char *cond = emit_expr(cg, node->if_expr.cond);
    if (is_else_if) {
        if (cond[0] == '(') {
            fprintf(cg->out, "if %s {\n", cond);
        } else {
            fprintf(cg->out, "if (%s) {\n", cond);
        }
    } else {
        if (cond[0] == '(') {
            emit_line(cg, "if %s {", cond);
        } else {
            emit_line(cg, "if (%s) {", cond);
        }
    }
    cg->indent++;

    emit_branch(cg, node->if_expr.then_body, target);
    cg->indent--;

    if (node->if_expr.else_body != NULL) {
        if (node->if_expr.else_body->kind == NODE_IF) {
            emit_indent(cg);
            fprintf(cg->out, "} else ");
            emit_if(cg, node->if_expr.else_body, target, true);
        } else {
            emit_line(cg, "} else {");
            cg->indent++;
            emit_branch(cg, node->if_expr.else_body, target);
            cg->indent--;
            emit_line(cg, "}");
        }
    } else {
        emit_line(cg, "}");
    }
}

// String conversion helper for interpolation
static const char *str_convert_expr(CodeGen *cg, const ASTNode *node) {
    const char *val = emit_expr(cg, node);
    const Type *t = node->type;
    if (t == NULL) {
        return val;
    }
    switch (t->kind) {
    case TYPE_STR:
        return val;
    case TYPE_I32:
        return arena_sprintf(cg->arena, "rg_str_from_i32(%s)", val);
    case TYPE_U32:
        return arena_sprintf(cg->arena, "rg_str_from_u32(%s)", val);
    case TYPE_F64:
        return arena_sprintf(cg->arena, "rg_str_from_f64(%s)", val);
    case TYPE_BOOL:
        return arena_sprintf(cg->arena, "rg_str_from_bool(%s)", val);
    default:
        return arena_sprintf(cg->arena, "rg_str_from_i32(%s)", val);
    }
}

// ---------------------------------------------------------------------------
// Per-node expression emitters
// ---------------------------------------------------------------------------
static const char *emit_literal_expr(CodeGen *cg, const ASTNode *node) {
    switch (node->literal.kind) {
    case LIT_BOOL:
        return node->literal.bool_val ? "true" : "false";
    case LIT_I32: {
        int64_t v = node->literal.int_val;
        if (v == (int64_t)(-2147483647 - 1)) {
            return "(-2147483647 - 1)";
        }
        return arena_sprintf(cg->arena, "%lld", (long long)v);
    }
    case LIT_U32:
        return arena_sprintf(cg->arena, "%lluU", (unsigned long long)node->literal.int_val);
    case LIT_F64:
        return fmt_f64(cg, node->literal.f64_val);
    case LIT_STR: {
        const char *escaped = c_str_escape(cg, node->literal.str_val);
        return arena_sprintf(cg->arena, "rg_str_lit(\"%s\")", escaped);
    }
    case LIT_UNIT:
        return "(void)0";
    }
    return "0";
}

static const char *emit_unary_expr(CodeGen *cg, const ASTNode *node) {
    // Fold negation into integer/float literals to avoid overflow issues
    if (node->unary.op == TOK_MINUS && node->unary.operand->kind == NODE_LITERAL) {
        const ASTNode *lit = node->unary.operand;
        if (lit->literal.kind == LIT_I32) {
            int64_t neg = -lit->literal.int_val;
            if (neg == (int64_t)(-2147483647 - 1)) {
                return "(-2147483647 - 1)";
            }
            return arena_sprintf(cg->arena, "%lld", (long long)neg);
        }
        if (lit->literal.kind == LIT_U32) {
            return arena_sprintf(cg->arena, "(-(int64_t)%lluU)", (unsigned long long)lit->literal.int_val);
        }
        if (lit->literal.kind == LIT_F64) {
            return fmt_f64(cg, -lit->literal.f64_val);
        }
    }
    const char *operand = emit_expr(cg, node->unary.operand);
    if (node->unary.op == TOK_BANG) {
        return arena_sprintf(cg->arena, "(!%s)", operand);
    }
    if (node->unary.op == TOK_MINUS) {
        return arena_sprintf(cg->arena, "(-%s)", operand);
    }
    return operand;
}

static const char *emit_binary_expr(CodeGen *cg, const ASTNode *node) {
    const ASTNode *lhs = node->binary.left;
    const ASTNode *rhs = node->binary.right;

    // Constant-fold binary operations on integer literals
    if (lhs->kind == NODE_LITERAL && rhs->kind == NODE_LITERAL && lhs->literal.kind == LIT_I32 &&
        rhs->literal.kind == LIT_I32) {
        int64_t a = lhs->literal.int_val;
        int64_t b = rhs->literal.int_val;
        int64_t result;
        bool folded = true;
        switch (node->binary.op) {
        case TOK_PLUS:
            result = a + b;
            break;
        case TOK_MINUS:
            result = a - b;
            break;
        case TOK_STAR:
            result = a * b;
            break;
        case TOK_SLASH:
            folded = (b != 0);
            if (folded) {
                result = a / b;
            }
            break;
        case TOK_PERCENT:
            folded = (b != 0);
            if (folded) {
                result = a % b;
            }
            break;
        default:
            folded = false;
            break;
        }
        if (folded) {
            if (result == (int64_t)(-2147483647 - 1)) {
                return "(-2147483647 - 1)";
            }
            return arena_sprintf(cg->arena, "%lld", (long long)result);
        }
    }

    const char *left = emit_expr(cg, lhs);
    const char *right = emit_expr(cg, rhs);

    // String equality/inequality
    const Type *lt = lhs->type;
    if (lt != NULL && lt->kind == TYPE_STR) {
        if (node->binary.op == TOK_EQ_EQ) {
            return arena_sprintf(cg->arena, "rg_str_eq(%s, %s)", left, right);
        }
        if (node->binary.op == TOK_BANG_EQ) {
            return arena_sprintf(cg->arena, "(!rg_str_eq(%s, %s))", left, right);
        }
    }

    return arena_sprintf(cg->arena, "(%s %s %s)", left, c_binop(node->binary.op), right);
}

static const char *emit_call_expr(CodeGen *cg, const ASTNode *node) {
    const char *callee;
    if (node->call.callee->kind == NODE_IDENT) {
        callee = mangle_fn_name(cg, node->call.callee->ident.name);
    } else {
        callee = emit_expr(cg, node->call.callee);
    }
    const char *args = "";
    for (int32_t i = 0; i < BUF_LEN(node->call.args); i++) {
        const char *arg = emit_expr(cg, node->call.args[i]);
        if (i == 0) {
            args = arg;
        } else {
            args = arena_sprintf(cg->arena, "%s, %s", args, arg);
        }
    }
    return arena_sprintf(cg->arena, "%s(%s)", callee, args);
}

static const char *emit_if_expr(CodeGen *cg, const ASTNode *node) {
    const Type *t = node->type;
    if (t == NULL || t->kind == TYPE_UNIT) {
        emit_if(cg, node, NULL, false);
        return "(void)0";
    }
    if (is_simple_ternary(node)) {
        const char *cond = emit_expr(cg, node->if_expr.cond);
        const ASTNode *then_result = simple_branch_result(node->if_expr.then_body);
        const ASTNode *else_result = simple_branch_result(node->if_expr.else_body);
        const char *then_val = emit_expr(cg, then_result);
        const char *else_val = emit_expr(cg, else_result);
        return arena_sprintf(cg->arena, "(%s ? %s : %s)", cond, then_val, else_val);
    }
    const char *tmp = next_tmp(cg);
    emit_line(cg, "%s %s;", c_type_str(t), tmp);
    emit_if(cg, node, tmp, false);
    return tmp;
}

static const char *emit_block_expr(CodeGen *cg, const ASTNode *node) {
    for (int32_t i = 0; i < BUF_LEN(node->block.stmts); i++) {
        emit_stmt(cg, node->block.stmts[i]);
    }
    if (node->block.result != NULL) {
        return emit_expr(cg, node->block.result);
    }
    return "(void)0";
}

static const char *emit_str_interp_expr(CodeGen *cg, const ASTNode *node) {
    int32_t part_count = BUF_LEN(node->str_interp.parts);
    const char **str_parts = NULL;
    for (int32_t i = 0; i < part_count; i++) {
        const ASTNode *part = node->str_interp.parts[i];
        if (part->kind == NODE_LITERAL && part->literal.kind == LIT_STR) {
            const char *text = part->literal.str_val;
            if (text == NULL || text[0] == '\0') {
                continue;
            }
            BUF_PUSH(str_parts, arena_sprintf(cg->arena, "rg_str_lit(\"%s\")", c_str_escape(cg, text)));
        } else {
            BUF_PUSH(str_parts, str_convert_expr(cg, part));
        }
    }
    int32_t n = BUF_LEN(str_parts);

    if (n == 1) {
        const char *result = str_parts[0];
        BUF_FREE(str_parts);
        return result;
    }
    if (n == 2) {
        const char *result = arena_sprintf(cg->arena, "rg_str_concat(%s, %s)", str_parts[0], str_parts[1]);
        BUF_FREE(str_parts);
        return result;
    }

    const char *sb = next_sb(cg);
    emit_line(cg, "RgStrBuilder %s;", sb);
    emit_line(cg, "rg_sb_init(&%s);", sb);
    for (int32_t i = 0; i < n; i++) {
        emit_line(cg, "rg_sb_append_str(&%s, %s);", sb, str_parts[i]);
    }
    BUF_FREE(str_parts);
    const char *tmp = next_tmp(cg);
    emit_line(cg, "RgStr %s = rg_sb_finish(&%s);", tmp, sb);
    return tmp;
}

// ---------------------------------------------------------------------------
// Expression emission — dispatch
// ---------------------------------------------------------------------------
static const char *emit_expr(CodeGen *cg, const ASTNode *node) {
    if (node == NULL) {
        return "0";
    }
    switch (node->kind) {
    case NODE_LITERAL:
        return emit_literal_expr(cg, node);
    case NODE_IDENT:
        return var_lookup(cg, node->ident.name);
    case NODE_UNARY:
        return emit_unary_expr(cg, node);
    case NODE_BINARY:
        return emit_binary_expr(cg, node);
    case NODE_CALL:
        return emit_call_expr(cg, node);
    case NODE_MEMBER:
        return arena_sprintf(cg->arena, "%s", node->member.member);
    case NODE_IF:
        return emit_if_expr(cg, node);
    case NODE_BLOCK:
        return emit_block_expr(cg, node);
    case NODE_STR_INTERP:
        return emit_str_interp_expr(cg, node);
    default:
        return "0";
    }
}

// ---------------------------------------------------------------------------
// Statement emission
// ---------------------------------------------------------------------------
static void emit_block_stmts(CodeGen *cg, const ASTNode *block) {
    if (block == NULL || block->kind != NODE_BLOCK) {
        return;
    }
    for (int32_t i = 0; i < BUF_LEN(block->block.stmts); i++) {
        emit_stmt(cg, block->block.stmts[i]);
    }
    // Note: block.result is NOT emitted here (caller handles it)
}

// ---------------------------------------------------------------------------
// Per-node statement emitters
// ---------------------------------------------------------------------------
static void emit_var_decl_stmt(CodeGen *cg, const ASTNode *node) {
    const Type *t = node->type;
    if (t == NULL) {
        t = node->var_decl.init != NULL ? node->var_decl.init->type : NULL;
    }
    if (t == NULL && node->var_decl.type.kind == AST_TYPE_NAME) {
        t = type_from_name(node->var_decl.type.name);
    }
    if (t == NULL) {
        t = &TYPE_I32_INST;
    }
    const char *c_name = var_define(cg, node->var_decl.name);
    const char *val = emit_expr(cg, node->var_decl.init);
    emit_line(cg, "%s %s = %s;", c_type_str(t), c_name, val);
}

static void emit_expr_stmt_body(CodeGen *cg, const ASTNode *node) {
    const ASTNode *expr = node->expr_stmt.expr;
    switch (expr->kind) {
    case NODE_IF:
        emit_if(cg, expr, NULL, false);
        break;
    case NODE_ASSIGN:
    case NODE_COMPOUND_ASSIGN:
        emit_stmt(cg, expr);
        break;
    default: {
        const char *val = emit_expr(cg, expr);
        emit_line(cg, "%s;", val);
        break;
    }
    }
}

static void emit_assign_stmt(CodeGen *cg, const ASTNode *node) {
    const char *target;
    if (node->assign.target->kind == NODE_IDENT) {
        target = var_lookup(cg, node->assign.target->ident.name);
    } else {
        target = emit_expr(cg, node->assign.target);
    }
    const char *value = emit_expr(cg, node->assign.value);
    emit_line(cg, "%s = %s;", target, value);
}

static void emit_compound_assign_stmt(CodeGen *cg, const ASTNode *node) {
    const char *target;
    if (node->compound_assign.target->kind == NODE_IDENT) {
        target = var_lookup(cg, node->compound_assign.target->ident.name);
    } else {
        target = emit_expr(cg, node->compound_assign.target);
    }
    const char *value = emit_expr(cg, node->compound_assign.value);
    emit_line(cg, "%s %s %s;", target, c_compound_op(node->compound_assign.op), value);
}

static void emit_assert_stmt(CodeGen *cg, const ASTNode *node) {
    const char *cond = emit_expr(cg, node->assert_stmt.cond);
    if (node->assert_stmt.message != NULL) {
        const char *msg;
        if (node->assert_stmt.message->kind == NODE_LITERAL && node->assert_stmt.message->literal.kind == LIT_STR) {
            msg = arena_sprintf(cg->arena, "\"%s\"", c_str_escape(cg, node->assert_stmt.message->literal.str_val));
        } else {
            const char *msg_expr = emit_expr(cg, node->assert_stmt.message);
            msg = arena_sprintf(cg->arena, "%s.data", msg_expr);
        }
        emit_line(cg, "rg_assert(%s, %s, _rg_file, %d);", cond, msg, node->loc.line);
    } else {
        emit_line(cg, "rg_assert(%s, NULL, _rg_file, %d);", cond, node->loc.line);
    }
}

static void emit_loop_stmt(CodeGen *cg, const ASTNode *node) {
    emit_line(cg, "while (1) {");
    cg->indent++;
    const ASTNode *body = node->loop.body;
    if (body != NULL && body->kind == NODE_BLOCK) {
        emit_block_stmts(cg, body);
        if (body->block.result != NULL) {
            emit_stmt(cg, body->block.result);
        }
    }
    cg->indent--;
    emit_line(cg, "}");
}

static void emit_for_stmt(CodeGen *cg, const ASTNode *node) {
    const char *start = emit_expr(cg, node->for_loop.start);
    const char *end = emit_expr(cg, node->for_loop.end);
    const char *var_c = var_define(cg, node->for_loop.var_name);
    emit_line(cg, "for (int32_t %s = %s; %s < %s; %s++) {", var_c, start, var_c, end, var_c);
    cg->indent++;
    if (node->for_loop.body != NULL && node->for_loop.body->kind == NODE_BLOCK) {
        emit_block_stmts(cg, node->for_loop.body);
        if (node->for_loop.body->block.result != NULL) {
            emit_stmt(cg, node->for_loop.body->block.result);
        }
    }
    cg->indent--;
    emit_line(cg, "}");
}

// ---------------------------------------------------------------------------
// Statement emission — dispatch
// ---------------------------------------------------------------------------
static void emit_stmt(CodeGen *cg, const ASTNode *node) {
    if (node == NULL) {
        return;
    }
    switch (node->kind) {
    case NODE_VAR_DECL:
        emit_var_decl_stmt(cg, node);
        break;
    case NODE_EXPR_STMT:
        emit_expr_stmt_body(cg, node);
        break;
    case NODE_ASSIGN:
        emit_assign_stmt(cg, node);
        break;
    case NODE_COMPOUND_ASSIGN:
        emit_compound_assign_stmt(cg, node);
        break;
    case NODE_ASSERT:
        emit_assert_stmt(cg, node);
        break;
    case NODE_BREAK:
        emit_line(cg, "break;");
        break;
    case NODE_CONTINUE:
        emit_line(cg, "continue;");
        break;
    case NODE_LOOP:
        emit_loop_stmt(cg, node);
        break;
    case NODE_FOR:
        emit_for_stmt(cg, node);
        break;
    case NODE_IF:
        emit_if(cg, node, NULL, false);
        break;
    case NODE_BLOCK:
        emit_line(cg, "{");
        cg->indent++;
        emit_block_stmts(cg, node);
        if (node->block.result != NULL) {
            emit_stmt(cg, node->block.result);
        }
        cg->indent--;
        emit_line(cg, "}");
        break;
    default: {
        const char *val = emit_expr(cg, node);
        emit_line(cg, "%s;", val);
        break;
    }
    }
}

// ---------------------------------------------------------------------------
// Function emission
// ---------------------------------------------------------------------------
static void emit_fn_body(CodeGen *cg, const ASTNode *fn_node) {
    const ASTNode *body = fn_node->fn_decl.body;
    const Type *ret = fn_node->type;
    bool is_unit = ret == NULL || ret->kind == TYPE_UNIT;
    bool is_main = strcmp(fn_node->fn_decl.name, "main") == 0;

    // Register parameters in var tracking
    for (int32_t i = 0; i < BUF_LEN(fn_node->fn_decl.params); i++) {
        const ASTNode *p = fn_node->fn_decl.params[i];
        var_define(cg, p->param.name);
    }

    if (body->kind == NODE_BLOCK) {
        // Block body with optional trailing result
        emit_block_stmts(cg, body);

        if (body->block.result != NULL) {
            const ASTNode *r = body->block.result;
            // Statement-like results need special handling
            if (r->kind == NODE_ASSIGN || r->kind == NODE_COMPOUND_ASSIGN) {
                // Emit as statement (side-effect only in function body)
                emit_stmt(cg, r);
            } else if (!is_unit && !is_main) {
                const char *result = emit_expr(cg, r);
                emit_line(cg, "return %s;", result);
            } else {
                // Unit/main: evaluate for side effects
                if (r->kind == NODE_CALL) {
                    const char *result = emit_expr(cg, r);
                    emit_line(cg, "%s;", result);
                } else if (r->kind == NODE_IF) {
                    emit_if(cg, r, NULL, false);
                } else {
                    const char *result = emit_expr(cg, r);
                    emit_line(cg, "(void)%s;", result);
                }
            }
        }
    } else {
        // Expression body (fn foo() = expr)
        const char *result = emit_expr(cg, body);
        if (!is_unit && !is_main) {
            emit_line(cg, "return %s;", result);
        } else {
            emit_line(cg, "(void)%s;", result);
        }
    }
}

static void emit_fn_decl(CodeGen *cg, const ASTNode *node, bool forward_only) {
    bool is_pub = node->fn_decl.is_pub;
    bool is_main = strcmp(node->fn_decl.name, "main") == 0;

    // Return type
    const char *ret_type = is_main ? "int" : c_type_str(node->type);

    // Static prefix for non-pub, non-main
    const char *prefix = (!is_pub && !is_main) ? "static " : "";

    // Mangled function name
    const char *fn_name = mangle_fn_name(cg, node->fn_decl.name);

    // Parameters
    emit_indent(cg);
    fprintf(cg->out, "%s%s %s(", prefix, ret_type, fn_name);

    int32_t param_count = BUF_LEN(node->fn_decl.params);
    if (param_count == 0) {
        fprintf(cg->out, "void");
    } else {
        for (int32_t i = 0; i < param_count; i++) {
            const ASTNode *p = node->fn_decl.params[i];
            const Type *pt = p->type;
            if (pt == NULL && p->param.type.kind == AST_TYPE_NAME) {
                pt = type_from_name(p->param.type.name);
            }
            if (pt == NULL) {
                pt = &TYPE_I32_INST;
            }
            if (i > 0) {
                fprintf(cg->out, ", ");
            }
            fprintf(cg->out, "%s %s", c_type_str(pt), p->param.name);
        }
    }

    if (forward_only) {
        fprintf(cg->out, ");\n");
        return;
    }

    fprintf(cg->out, ") {\n");
    cg->indent++;
    var_scope_reset(cg);
    emit_fn_body(cg, node);

    if (is_main) {
        emit_line(cg, "return 0;");
    }

    cg->indent--;
    emit_line(cg, "}");
    fprintf(cg->out, "\n");
}

// ---------------------------------------------------------------------------
// File emission
// ---------------------------------------------------------------------------
static void emit_preamble(CodeGen *cg) {
    emit(cg, "// Generated by resurg compiler — do not edit.\n");
    emit(cg, "#include <stdint.h>\n");
    emit(cg, "#include <stdbool.h>\n");
    emit(cg, "#include \"runtime.h\"\n\n");
}

static void emit_file(CodeGen *cg, const ASTNode *file) {
    emit_preamble(cg);

    // Emit source file path constant (used by rg_assert)
    cg->source_file = c_escape_path(cg, file->loc.file);
    emit(cg, "static const char *_rg_file = \"%s\";\n\n", cg->source_file);

    // Emit module comment
    for (int32_t i = 0; i < BUF_LEN(file->file.decls); i++) {
        const ASTNode *d = file->file.decls[i];
        if (d->kind == NODE_MODULE) {
            cg->module = d->module.name;
            emit(cg, "// module %s\n\n", d->module.name);
        }
    }

    // Forward declarations for all functions
    for (int32_t i = 0; i < BUF_LEN(file->file.decls); i++) {
        const ASTNode *d = file->file.decls[i];
        if (d->kind == NODE_FN_DECL) {
            emit_fn_decl(cg, d, true);
        }
    }
    emit(cg, "\n");

    // Full function definitions
    for (int32_t i = 0; i < BUF_LEN(file->file.decls); i++) {
        const ASTNode *d = file->file.decls[i];
        if (d->kind == NODE_FN_DECL) {
            emit_fn_decl(cg, d, false);
        }
    }

    // Top-level statements (outside functions) — wrap in a helper if needed
    bool has_top_stmts = false;
    for (int32_t i = 0; i < BUF_LEN(file->file.decls); i++) {
        const ASTNode *d = file->file.decls[i];
        if (d->kind != NODE_MODULE && d->kind != NODE_FN_DECL) {
            has_top_stmts = true;
            break;
        }
    }
    if (has_top_stmts) {
        emit(cg, "static void _rg_top_level(void) {\n");
        cg->indent++;
        for (int32_t i = 0; i < BUF_LEN(file->file.decls); i++) {
            const ASTNode *d = file->file.decls[i];
            if (d->kind != NODE_MODULE && d->kind != NODE_FN_DECL) {
                emit_stmt(cg, d);
            }
        }
        cg->indent--;
        emit(cg, "}\n\n");
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
CodeGen *codegen_create(FILE *out, Arena *arena) {
    CodeGen *cg = malloc(sizeof(*cg));
    if (cg == NULL) {
        rg_fatal("out of memory");
    }
    cg->out = out;
    cg->arena = arena;
    cg->indent = 0;
    cg->module = NULL;
    cg->source_file = NULL;
    cg->tmp_counter = 0;
    cg->sb_counter = 0;
    cg->vars = NULL;
    cg->shadow_counter = 0;
    return cg;
}

void codegen_destroy(CodeGen *cg) {
    if (cg != NULL) {
        BUF_FREE(cg->vars);
        free(cg);
    }
}

void codegen_emit(CodeGen *cg, const ASTNode *file) {
    emit_file(cg, file);
}
