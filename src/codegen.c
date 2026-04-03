#include "codegen.h"

// ------------------------------------------------------------------------
// VarEntry — private implementation (opaque in codegen.h)
// ------------------------------------------------------------------------
struct VarEntry {
    const char *resurg_name; // Resurg name
    const char *c_name;      // C name (may be mangled for shadowing)
};

struct CodeGen {
    FILE *out;                       // output file handle
    Arena *arena;                    // for temporary string building
    int32_t indent;                  // current indentation level
    const char *module;              // may be NULL
    const char *source_file;         // may be NULL until emit_file
    int32_t temporary_counter;       // counter for temp variable names
    int32_t string_builder_counter;  // counter for string builder names
    VarEntry *vars;                  /* buf */
    int32_t shadow_variable_counter; // counter for shadow name mangling
};

// ------------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------------
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

static const char *next_temporary(CodeGen *cg) {
    return arena_sprintf(cg->arena, "_rg_tmp_%d", cg->temporary_counter++);
}

static const char *next_string_builder(CodeGen *cg) {
    return arena_sprintf(cg->arena, "_rg_sb_%d", cg->string_builder_counter++);
}

// ------------------------------------------------------------------------
// Variable name tracking (for shadowed variables)
// ------------------------------------------------------------------------
static int32_t variable_find(const CodeGen *cg, const char *name) {
    for (int32_t i = BUF_LEN(cg->vars) - 1; i >= 0; i--) {
        if (strcmp(cg->vars[i].resurg_name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static const char *variable_lookup(const CodeGen *cg, const char *name) {
    int32_t index = variable_find(cg, name);
    return (index >= 0) ? cg->vars[index].c_name : name;
}

static const char *variable_define(CodeGen *cg, const char *name) {
    const char *c_name =
        (variable_find(cg, name) >= 0) ? arena_sprintf(cg->arena, "%s__%d", name, cg->shadow_variable_counter++) : name;
    VarEntry entry = {name, c_name};
    BUF_PUSH(cg->vars, entry);
    return c_name;
}

static void variable_scope_reset(CodeGen *cg) {
    if (cg->vars != NULL) {
        BUF_FREE(cg->vars);
        cg->vars = NULL;
    }
    cg->shadow_variable_counter = 0;
}

// ------------------------------------------------------------------------
// Function name mangling (avoid C reserved words / stdlib conflicts)
// ------------------------------------------------------------------------
static const char *mangle_fn_name(CodeGen *cg, const char *name) {
    if (strcmp(name, "main") == 0) {
        return "main";
    }
    return arena_sprintf(cg->arena, "rg_%s", name);
}

// ------------------------------------------------------------------------
// Forward declarations
// ------------------------------------------------------------------------
static const char *emit_expression(CodeGen *cg, const ASTNode *node);
static void emit_statement(CodeGen *cg, const ASTNode *node);
static void emit_block_statements(CodeGen *cg, const ASTNode *block);

// ------------------------------------------------------------------------
// Helpers for ternary optimization
// ------------------------------------------------------------------------
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

// Check whether emit_expression on this node is pure (returns a C expression string
// without emitting any lines to the output file).
static bool is_pure_expression(const ASTNode *node) {
    if (node == NULL) {
        return false;
    }
    switch (node->kind) {
    case NODE_LITERAL:
    case NODE_IDENT:
        return true;
    case NODE_UNARY:
        return is_pure_expression(node->unary.operand);
    case NODE_BINARY:
        return is_pure_expression(node->binary.left) && is_pure_expression(node->binary.right);
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
    if (then_result == NULL || !is_pure_expression(then_result)) {
        return false;
    }
    const ASTNode *else_body = node->if_expr.else_body;
    if (else_body->kind == NODE_IF) {
        return false; // else-if chains stay as statements
    }
    const ASTNode *else_result = simple_branch_result(else_body);
    return else_result != NULL && is_pure_expression(else_result);
}

// ------------------------------------------------------------------------
// C operator string
// ------------------------------------------------------------------------
static const char *c_binary_operator(TokenKind op) {
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

static const char *c_compound_operator(TokenKind op) {
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

// ------------------------------------------------------------------------
// Escape a string for C string literal output
// ------------------------------------------------------------------------
static const char *c_string_escape(const CodeGen *cg, const char *s) {
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
    int32_t length = (int32_t)strlen(s);
    char *buffer = arena_alloc(cg->arena, length + extra + 1);
    int32_t j = 0;
    for (const char *p = s; *p != '\0'; p++) {
        switch (*p) {
        case '\\':
            buffer[j++] = '\\';
            buffer[j++] = '\\';
            break;
        case '"':
            buffer[j++] = '\\';
            buffer[j++] = '"';
            break;
        case '\n':
            buffer[j++] = '\\';
            buffer[j++] = 'n';
            break;
        case '\r':
            buffer[j++] = '\\';
            buffer[j++] = 'r';
            break;
        case '\t':
            buffer[j++] = '\\';
            buffer[j++] = 't';
            break;
        default:
            buffer[j++] = *p;
            break;
        }
    }
    buffer[j] = '\0';
    return buffer;
}

// Escape a file path for embedding in C string (backslash -> forward slash)
static const char *c_escape_file_path(const CodeGen *cg, const char *path) {
    if (path == NULL) {
        return "";
    }
    int32_t length = (int32_t)strlen(path);
    char *buffer = arena_alloc(cg->arena, length + 1);
    for (int32_t i = 0; i < length; i++) {
        buffer[i] = (char)((path[i] == '\\') ? '/' : path[i]);
    }
    buffer[length] = '\0';
    return buffer;
}

// Format a double as a C literal string (ensures trailing .0 if needed)
static const char *format_float64(const CodeGen *cg, double v) {
    char buffer[64];
    int32_t length = snprintf(buffer, sizeof(buffer), "%.17g", v);
    bool has_dot = false;
    for (int32_t i = 0; i < length; i++) {
        if (buffer[i] == '.' || buffer[i] == 'e' || buffer[i] == 'E') {
            has_dot = true;
            break;
        }
    }
    if (!has_dot) {
        buffer[length] = '.';
        buffer[length + 1] = '0';
        buffer[length + 2] = '\0';
    }
    return arena_strdup(cg->arena, buffer);
}

// ------------------------------------------------------------------------
// Expression emission — returns a C expression string
// ------------------------------------------------------------------------
// Emit an if/else branch body, optionally assigning the result to a target
// variable.
static void emit_branch(CodeGen *cg, const ASTNode *body, const char *target) {
    if (body->kind == NODE_BLOCK) {
        if (target != NULL) {
            for (int32_t i = 0; i < BUF_LEN(body->block.stmts); i++) {
                emit_statement(cg, body->block.stmts[i]);
            }
            if (body->block.result != NULL) {
                const char *v = emit_expression(cg, body->block.result);
                emit_line(cg, "%s = %s;", target, v);
            }
        } else {
            emit_block_statements(cg, body);
            if (body->block.result != NULL) {
                emit_statement(cg, body->block.result);
            }
        }
    } else {
        if (target != NULL) {
            const char *v = emit_expression(cg, body);
            emit_line(cg, "%s = %s;", target, v);
        } else {
            emit_statement(cg, body);
        }
    }
}

// Emit an if-expression or if-statement.
// If target is non-NULL, assigns result to target (expression mode).
// If target is NULL, emits as statement (no result value).
static void emit_if(CodeGen *cg, const ASTNode *node, const char *target, bool is_else_if) {
    const char *cond_value = emit_expression(cg, node->if_expr.condition);
    if (is_else_if) {
        if (cond_value[0] == '(') {
            fprintf(cg->out, "if %s {\n", cond_value);
        } else {
            fprintf(cg->out, "if (%s) {\n", cond_value);
        }
    } else {
        if (cond_value[0] == '(') {
            emit_line(cg, "if %s {", cond_value);
        } else {
            emit_line(cg, "if (%s) {", cond_value);
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
static const char *string_convert_expression(CodeGen *cg, const ASTNode *node) {
    const char *value = emit_expression(cg, node);
    const Type *t = node->type;
    if (t == NULL) {
        return value;
    }
    switch (t->kind) {
    case TYPE_STR:
        return value;
    case TYPE_I32:
        return arena_sprintf(cg->arena, "rg_str_from_i32(%s)", value);
    case TYPE_U32:
        return arena_sprintf(cg->arena, "rg_str_from_u32(%s)", value);
    case TYPE_F64:
        return arena_sprintf(cg->arena, "rg_str_from_f64(%s)", value);
    case TYPE_BOOL:
        return arena_sprintf(cg->arena, "rg_str_from_bool(%s)", value);
    default:
        return arena_sprintf(cg->arena, "rg_str_from_i32(%s)", value);
    }
}

// ------------------------------------------------------------------------
// Per-node expression emitters
// ------------------------------------------------------------------------
static const char *emit_literal_expression(CodeGen *cg, const ASTNode *node) {
    switch (node->literal.kind) {
    case LIT_BOOL:
        return node->literal.boolean_value ? "true" : "false";
    case LIT_I32: {
        int64_t v = node->literal.integer_value;
        if (v == (int64_t)(-2147483647 - 1)) {
            return "(-2147483647 - 1)";
        }
        return arena_sprintf(cg->arena, "%lld", (long long)v);
    }
    case LIT_U32:
        return arena_sprintf(cg->arena, "%lluU", (unsigned long long)node->literal.integer_value);
    case LIT_F64:
        return format_float64(cg, node->literal.float64_value);
    case LIT_STR: {
        const char *escaped = c_string_escape(cg, node->literal.string_value);
        return arena_sprintf(cg->arena, "rg_str_lit(\"%s\")", escaped);
    }
    case LIT_UNIT:
        return "(void)0";
    }
    return "0";
}

static const char *emit_unary_expression(CodeGen *cg, const ASTNode *node) {
    // Fold negation into integer/float literals to avoid overflow issues
    if (node->unary.op == TOK_MINUS && node->unary.operand->kind == NODE_LITERAL) {
        const ASTNode *lit = node->unary.operand;
        if (lit->literal.kind == LIT_I32) {
            int64_t neg = -lit->literal.integer_value;
            if (neg == (int64_t)(-2147483647 - 1)) {
                return "(-2147483647 - 1)";
            }
            return arena_sprintf(cg->arena, "%lld", (long long)neg);
        }
        if (lit->literal.kind == LIT_U32) {
            return arena_sprintf(cg->arena, "(-(int64_t)%lluU)", (unsigned long long)lit->literal.integer_value);
        }
        if (lit->literal.kind == LIT_F64) {
            return format_float64(cg, -lit->literal.float64_value);
        }
    }
    const char *operand = emit_expression(cg, node->unary.operand);
    if (node->unary.op == TOK_BANG) {
        return arena_sprintf(cg->arena, "(!%s)", operand);
    }
    if (node->unary.op == TOK_MINUS) {
        return arena_sprintf(cg->arena, "(-%s)", operand);
    }
    return operand;
}

static const char *emit_binary_expression(CodeGen *cg, const ASTNode *node) {
    const ASTNode *left_operand = node->binary.left;
    const ASTNode *right_operand = node->binary.right;

    // Constant-fold binary operations on integer literals
    if (left_operand->kind == NODE_LITERAL && right_operand->kind == NODE_LITERAL &&
        left_operand->literal.kind == LIT_I32 && right_operand->literal.kind == LIT_I32) {
        int64_t a = left_operand->literal.integer_value;
        int64_t b = right_operand->literal.integer_value;
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

    const char *left = emit_expression(cg, left_operand);
    const char *right = emit_expression(cg, right_operand);

    // String equality/inequality
    const Type *left_type = left_operand->type;
    if (left_type != NULL && left_type->kind == TYPE_STR) {
        if (node->binary.op == TOK_EQ_EQ) {
            return arena_sprintf(cg->arena, "rg_str_eq(%s, %s)", left, right);
        }
        if (node->binary.op == TOK_BANG_EQ) {
            return arena_sprintf(cg->arena, "(!rg_str_eq(%s, %s))", left, right);
        }
    }

    return arena_sprintf(cg->arena, "(%s %s %s)", left, c_binary_operator(node->binary.op), right);
}

static const char *emit_call_expression(CodeGen *cg, const ASTNode *node) {
    const char *callee;
    if (node->call.callee->kind == NODE_IDENT) {
        callee = mangle_fn_name(cg, node->call.callee->ident.name);
    } else {
        callee = emit_expression(cg, node->call.callee);
    }
    const char *arguments = "";
    for (int32_t i = 0; i < BUF_LEN(node->call.args); i++) {
        const char *argument = emit_expression(cg, node->call.args[i]);
        if (i == 0) {
            arguments = argument;
        } else {
            arguments = arena_sprintf(cg->arena, "%s, %s", arguments, argument);
        }
    }
    return arena_sprintf(cg->arena, "%s(%s)", callee, arguments);
}

static const char *emit_if_expression(CodeGen *cg, const ASTNode *node) {
    const Type *t = node->type;
    if (t == NULL || t->kind == TYPE_UNIT) {
        emit_if(cg, node, NULL, false);
        return "(void)0";
    }
    if (is_simple_ternary(node)) {
        const char *condition = emit_expression(cg, node->if_expr.condition);
        const ASTNode *then_result = simple_branch_result(node->if_expr.then_body);
        const ASTNode *else_result = simple_branch_result(node->if_expr.else_body);
        const char *then_value = emit_expression(cg, then_result);
        const char *else_value = emit_expression(cg, else_result);
        return arena_sprintf(cg->arena, "(%s ? %s : %s)", condition, then_value, else_value);
    }
    const char *temporary = next_temporary(cg);
    emit_line(cg, "%s %s;", c_type_str(t), temporary);
    emit_if(cg, node, temporary, false);
    return temporary;
}

static const char *emit_block_expression(CodeGen *cg, const ASTNode *node) {
    for (int32_t i = 0; i < BUF_LEN(node->block.stmts); i++) {
        emit_statement(cg, node->block.stmts[i]);
    }
    if (node->block.result != NULL) {
        return emit_expression(cg, node->block.result);
    }
    return "(void)0";
}

static const char *emit_string_interpolation_expression(CodeGen *cg, const ASTNode *node) {
    int32_t part_count = BUF_LEN(node->str_interp.parts);
    const char **string_parts = NULL;
    for (int32_t i = 0; i < part_count; i++) {
        const ASTNode *part = node->str_interp.parts[i];
        if (part->kind == NODE_LITERAL && part->literal.kind == LIT_STR) {
            const char *text = part->literal.string_value;
            if (text == NULL || text[0] == '\0') {
                continue;
            }
            BUF_PUSH(string_parts, arena_sprintf(cg->arena, "rg_str_lit(\"%s\")", c_string_escape(cg, text)));
        } else {
            BUF_PUSH(string_parts, string_convert_expression(cg, part));
        }
    }
    int32_t n = BUF_LEN(string_parts);

    if (n == 1) {
        const char *result = string_parts[0];
        BUF_FREE(string_parts);
        return result;
    }
    if (n == 2) {
        const char *result = arena_sprintf(cg->arena, "rg_str_concat(%s, %s)", string_parts[0], string_parts[1]);
        BUF_FREE(string_parts);
        return result;
    }

    const char *builder = next_string_builder(cg);
    emit_line(cg, "RgStrBuilder %s;", builder);
    emit_line(cg, "rg_sb_init(&%s);", builder);
    for (int32_t i = 0; i < n; i++) {
        emit_line(cg, "rg_sb_append_str(&%s, %s);", builder, string_parts[i]);
    }
    BUF_FREE(string_parts);
    const char *temporary = next_temporary(cg);
    emit_line(cg, "RgStr %s = rg_sb_finish(&%s);", temporary, builder);
    return temporary;
}

// ------------------------------------------------------------------------
// Expression emission — dispatch
// ------------------------------------------------------------------------
static const char *emit_expression(CodeGen *cg, const ASTNode *node) {
    if (node == NULL) {
        return "0";
    }
    switch (node->kind) {
    case NODE_LITERAL:
        return emit_literal_expression(cg, node);
    case NODE_IDENT:
        return variable_lookup(cg, node->ident.name);
    case NODE_UNARY:
        return emit_unary_expression(cg, node);
    case NODE_BINARY:
        return emit_binary_expression(cg, node);
    case NODE_CALL:
        return emit_call_expression(cg, node);
    case NODE_MEMBER:
        return arena_sprintf(cg->arena, "%s", node->member.member);
    case NODE_IF:
        return emit_if_expression(cg, node);
    case NODE_BLOCK:
        return emit_block_expression(cg, node);
    case NODE_STR_INTERP:
        return emit_string_interpolation_expression(cg, node);
    default:
        return "0";
    }
}

// ------------------------------------------------------------------------
// Statement emission
// ------------------------------------------------------------------------
static void emit_block_statements(CodeGen *cg, const ASTNode *block) {
    if (block == NULL || block->kind != NODE_BLOCK) {
        return;
    }
    for (int32_t i = 0; i < BUF_LEN(block->block.stmts); i++) {
        emit_statement(cg, block->block.stmts[i]);
    }
    // Note: block.result is NOT emitted here (caller handles it)
}

// ------------------------------------------------------------------------
// Per-node statement emitters
// ------------------------------------------------------------------------
static void emit_variable_declaration_statement(CodeGen *cg, const ASTNode *node) {
    const Type *t = node->type;
    if (t == NULL) {
        t = node->var_decl.initializer != NULL ? node->var_decl.initializer->type : NULL;
    }
    if (t == NULL && node->var_decl.type.kind == AST_TYPE_NAME) {
        t = type_from_name(node->var_decl.type.name);
    }
    if (t == NULL) {
        t = &TYPE_I32_INST;
    }
    const char *c_name = variable_define(cg, node->var_decl.name);
    const char *value = emit_expression(cg, node->var_decl.initializer);
    emit_line(cg, "%s %s = %s;", c_type_str(t), c_name, value);
}

static void emit_expression_statement_body(CodeGen *cg, const ASTNode *node) {
    const ASTNode *expression = node->expr_stmt.expr;
    switch (expression->kind) {
    case NODE_IF:
        emit_if(cg, expression, NULL, false);
        break;
    case NODE_ASSIGN:
    case NODE_COMPOUND_ASSIGN:
        emit_statement(cg, expression);
        break;
    default: {
        const char *value = emit_expression(cg, expression);
        emit_line(cg, "%s;", value);
        break;
    }
    }
}

static void emit_assign_statement(CodeGen *cg, const ASTNode *node) {
    const char *target;
    if (node->assign.target->kind == NODE_IDENT) {
        target = variable_lookup(cg, node->assign.target->ident.name);
    } else {
        target = emit_expression(cg, node->assign.target);
    }
    const char *value = emit_expression(cg, node->assign.value);
    emit_line(cg, "%s = %s;", target, value);
}

static void emit_compound_assign_statement(CodeGen *cg, const ASTNode *node) {
    const char *target;
    if (node->compound_assign.target->kind == NODE_IDENT) {
        target = variable_lookup(cg, node->compound_assign.target->ident.name);
    } else {
        target = emit_expression(cg, node->compound_assign.target);
    }
    const char *value = emit_expression(cg, node->compound_assign.value);
    emit_line(cg, "%s %s %s;", target, c_compound_operator(node->compound_assign.op), value);
}

static void emit_assert_statement(CodeGen *cg, const ASTNode *node) {
    const char *condition = emit_expression(cg, node->assert_stmt.condition);
    if (node->assert_stmt.message != NULL) {
        const char *message;
        if (node->assert_stmt.message->kind == NODE_LITERAL && node->assert_stmt.message->literal.kind == LIT_STR) {
            message = arena_sprintf(cg->arena, "\"%s\"",
                                    c_string_escape(cg, node->assert_stmt.message->literal.string_value));
        } else {
            const char *message_expression = emit_expression(cg, node->assert_stmt.message);
            message = arena_sprintf(cg->arena, "%s.data", message_expression);
        }
        emit_line(cg, "rg_assert(%s, %s, _rg_file, %d);", condition, message, node->loc.line);
    } else {
        emit_line(cg, "rg_assert(%s, NULL, _rg_file, %d);", condition, node->loc.line);
    }
}

static void emit_loop_statement(CodeGen *cg, const ASTNode *node) {
    emit_line(cg, "while (1) {");
    cg->indent++;
    const ASTNode *body = node->loop.body;
    if (body != NULL && body->kind == NODE_BLOCK) {
        emit_block_statements(cg, body);
        if (body->block.result != NULL) {
            emit_statement(cg, body->block.result);
        }
    }
    cg->indent--;
    emit_line(cg, "}");
}

static void emit_for_statement(CodeGen *cg, const ASTNode *node) {
    const char *start = emit_expression(cg, node->for_loop.start);
    const char *end = emit_expression(cg, node->for_loop.end);
    const char *variable_c = variable_define(cg, node->for_loop.var_name);
    emit_line(cg, "for (int32_t %s = %s; %s < %s; %s++) {", variable_c, start, variable_c, end, variable_c);
    cg->indent++;
    if (node->for_loop.body != NULL && node->for_loop.body->kind == NODE_BLOCK) {
        emit_block_statements(cg, node->for_loop.body);
        if (node->for_loop.body->block.result != NULL) {
            emit_statement(cg, node->for_loop.body->block.result);
        }
    }
    cg->indent--;
    emit_line(cg, "}");
}

// ------------------------------------------------------------------------
// Statement emission — dispatch
// ------------------------------------------------------------------------
static void emit_statement(CodeGen *cg, const ASTNode *node) {
    if (node == NULL) {
        return;
    }
    switch (node->kind) {
    case NODE_VAR_DECL:
        emit_variable_declaration_statement(cg, node);
        break;
    case NODE_EXPR_STMT:
        emit_expression_statement_body(cg, node);
        break;
    case NODE_ASSIGN:
        emit_assign_statement(cg, node);
        break;
    case NODE_COMPOUND_ASSIGN:
        emit_compound_assign_statement(cg, node);
        break;
    case NODE_ASSERT:
        emit_assert_statement(cg, node);
        break;
    case NODE_BREAK:
        emit_line(cg, "break;");
        break;
    case NODE_CONTINUE:
        emit_line(cg, "continue;");
        break;
    case NODE_LOOP:
        emit_loop_statement(cg, node);
        break;
    case NODE_FOR:
        emit_for_statement(cg, node);
        break;
    case NODE_IF:
        emit_if(cg, node, NULL, false);
        break;
    case NODE_BLOCK:
        emit_line(cg, "{");
        cg->indent++;
        emit_block_statements(cg, node);
        if (node->block.result != NULL) {
            emit_statement(cg, node->block.result);
        }
        cg->indent--;
        emit_line(cg, "}");
        break;
    default: {
        const char *value = emit_expression(cg, node);
        emit_line(cg, "%s;", value);
        break;
    }
    }
}

// ------------------------------------------------------------------------
// Function emission
// ------------------------------------------------------------------------
static void emit_function_body(CodeGen *cg, const ASTNode *fn_node) {
    const ASTNode *body = fn_node->fn_decl.body;
    const Type *return_type = fn_node->type;
    bool is_unit = return_type == NULL || return_type->kind == TYPE_UNIT;
    bool is_main = strcmp(fn_node->fn_decl.name, "main") == 0;

    // Register parameters in variable tracking
    for (int32_t i = 0; i < BUF_LEN(fn_node->fn_decl.params); i++) {
        const ASTNode *parameter = fn_node->fn_decl.params[i];
        variable_define(cg, parameter->param.name);
    }

    if (body->kind == NODE_BLOCK) {
        // Block body with optional trailing result
        emit_block_statements(cg, body);

        if (body->block.result != NULL) {
            const ASTNode *result_node = body->block.result;
            // Statement-like results need special handling
            if (result_node->kind == NODE_ASSIGN || result_node->kind == NODE_COMPOUND_ASSIGN) {
                // Emit as statement (side-effect only in function body)
                emit_statement(cg, result_node);
            } else if (!is_unit && !is_main) {
                const char *result = emit_expression(cg, result_node);
                emit_line(cg, "return %s;", result);
            } else {
                // Unit/main: evaluate for side effects
                if (result_node->kind == NODE_CALL) {
                    const char *result = emit_expression(cg, result_node);
                    emit_line(cg, "%s;", result);
                } else if (result_node->kind == NODE_IF) {
                    emit_if(cg, result_node, NULL, false);
                } else {
                    const char *result = emit_expression(cg, result_node);
                    emit_line(cg, "(void)%s;", result);
                }
            }
        }
    } else {
        // Expression body (fn foo() = expr)
        const char *result = emit_expression(cg, body);
        if (!is_unit && !is_main) {
            emit_line(cg, "return %s;", result);
        } else {
            emit_line(cg, "(void)%s;", result);
        }
    }
}

static void emit_function_declaration(CodeGen *cg, const ASTNode *node, bool forward_only) {
    bool is_pub = node->fn_decl.is_pub;
    bool is_main = strcmp(node->fn_decl.name, "main") == 0;

    // Return type
    const char *return_type = is_main ? "int" : c_type_str(node->type);

    // Static prefix for non-pub, non-main
    const char *prefix = (!is_pub && !is_main) ? "static " : "";

    // Mangled function name
    const char *function_name = mangle_fn_name(cg, node->fn_decl.name);

    // Parameters
    emit_indent(cg);
    fprintf(cg->out, "%s%s %s(", prefix, return_type, function_name);

    int32_t parameter_count = BUF_LEN(node->fn_decl.params);
    if (parameter_count == 0) {
        fprintf(cg->out, "void");
    } else {
        for (int32_t i = 0; i < parameter_count; i++) {
            const ASTNode *parameter = node->fn_decl.params[i];
            const Type *parameter_type = parameter->type;
            if (parameter_type == NULL && parameter->param.type.kind == AST_TYPE_NAME) {
                parameter_type = type_from_name(parameter->param.type.name);
            }
            if (parameter_type == NULL) {
                parameter_type = &TYPE_I32_INST;
            }
            if (i > 0) {
                fprintf(cg->out, ", ");
            }
            fprintf(cg->out, "%s %s", c_type_str(parameter_type), parameter->param.name);
        }
    }

    if (forward_only) {
        fprintf(cg->out, ");\n");
        return;
    }

    fprintf(cg->out, ") {\n");
    cg->indent++;
    variable_scope_reset(cg);
    emit_function_body(cg, node);

    if (is_main) {
        emit_line(cg, "return 0;");
    }

    cg->indent--;
    emit_line(cg, "}");
    fprintf(cg->out, "\n");
}

// ------------------------------------------------------------------------
// File emission
// ------------------------------------------------------------------------
static void emit_preamble(CodeGen *cg) {
    emit(cg, "// Generated by resurg compiler — do not edit.\n");
    emit(cg, "#include <stdint.h>\n");
    emit(cg, "#include <stdbool.h>\n");
    emit(cg, "#include \"runtime.h\"\n\n");
}

static void emit_file(CodeGen *cg, const ASTNode *file) {
    emit_preamble(cg);

    // Emit source file path constant (used by rg_assert)
    cg->source_file = c_escape_file_path(cg, file->loc.file);
    emit(cg, "static const char *_rg_file = \"%s\";\n\n", cg->source_file);

    // Emit module comment
    for (int32_t i = 0; i < BUF_LEN(file->file.decls); i++) {
        const ASTNode *declaration = file->file.decls[i];
        if (declaration->kind == NODE_MODULE) {
            cg->module = declaration->module.name;
            emit(cg, "// module %s\n\n", declaration->module.name);
        }
    }

    // Forward declarations for all functions
    for (int32_t i = 0; i < BUF_LEN(file->file.decls); i++) {
        const ASTNode *declaration = file->file.decls[i];
        if (declaration->kind == NODE_FN_DECL) {
            emit_function_declaration(cg, declaration, true);
        }
    }
    emit(cg, "\n");

    // Full function definitions
    for (int32_t i = 0; i < BUF_LEN(file->file.decls); i++) {
        const ASTNode *declaration = file->file.decls[i];
        if (declaration->kind == NODE_FN_DECL) {
            emit_function_declaration(cg, declaration, false);
        }
    }

    // Top-level statements (outside functions) — wrap in a helper if needed
    bool has_top_statements = false;
    for (int32_t i = 0; i < BUF_LEN(file->file.decls); i++) {
        const ASTNode *declaration = file->file.decls[i];
        if (declaration->kind != NODE_MODULE && declaration->kind != NODE_FN_DECL) {
            has_top_statements = true;
            break;
        }
    }
    if (has_top_statements) {
        emit(cg, "static void _rg_top_level(void) {\n");
        cg->indent++;
        for (int32_t i = 0; i < BUF_LEN(file->file.decls); i++) {
            const ASTNode *declaration = file->file.decls[i];
            if (declaration->kind != NODE_MODULE && declaration->kind != NODE_FN_DECL) {
                emit_statement(cg, declaration);
            }
        }
        cg->indent--;
        emit(cg, "}\n\n");
    }
}

// ------------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------------
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
    cg->temporary_counter = 0;
    cg->string_builder_counter = 0;
    cg->vars = NULL;
    cg->shadow_variable_counter = 0;
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
