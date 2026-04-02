#include "parser.h"

// ------------------------------------------------------------------------
// Private struct definition
// ------------------------------------------------------------------------
struct Parser {
    const Token *tokens; // stretchy buffer from lexer (read-only)
    int32_t pos;         // current position
    int32_t count;       // total token count
    Arena *arena;        // AST allocation arena
    const char *file;    // source file name (for diagnostics)
};

// ------------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------------
static const Token *current(const Parser *p) {
    return &p->tokens[p->pos];
}

static const Token *previous(const Parser *p) {
    return &p->tokens[p->pos - 1];
}

static bool at_end(const Parser *p) {
    return current(p)->kind == TOK_EOF;
}

static bool check(const Parser *p, TokenKind kind) {
    return current(p)->kind == kind;
}

static const Token *advance_tok(Parser *p) {
    if (!at_end(p)) {
        p->pos++;
    }
    return previous(p);
}

static bool match(Parser *p, TokenKind kind) {
    if (check(p, kind)) {
        advance_tok(p);
        return true;
    }
    return false;
}

static const Token *expect(Parser *p, TokenKind kind) {
    if (check(p, kind)) {
        return advance_tok(p);
    }
    rg_error(current(p)->loc, "expected '%s', got '%s'", token_kind_str(kind), token_kind_str(current(p)->kind));
    return current(p);
}

static void skip_newlines(Parser *p) {
    while (check(p, TOK_NEWLINE)) {
        advance_tok(p);
    }
}

static SrcLoc loc(const Parser *p) {
    return current(p)->loc;
}

// ------------------------------------------------------------------------
// Forward declarations
// ------------------------------------------------------------------------
static ASTNode *parse_expr(Parser *p);
static ASTNode *parse_stmt(Parser *p);
static ASTNode *parse_block(Parser *p);
static ASTNode *parse_decl(Parser *p);

// ------------------------------------------------------------------------
// Type annotation
// ------------------------------------------------------------------------
static ASTType parse_type(Parser *p) {
    ASTType t = {.kind = AST_TYPE_NAME, .loc = loc(p)};

    switch (current(p)->kind) {
    case TOK_BOOL:
    case TOK_I32:
    case TOK_U32:
    case TOK_F64:
    case TOK_STR:
    case TOK_UNIT:
    case TOK_IDENT:
        t.name = advance_tok(p)->lexeme;
        break;
    default:
        rg_error(loc(p), "expected type name");
        t.kind = AST_TYPE_INFERRED;
        break;
    }
    return t;
}

// ------------------------------------------------------------------------
// Expressions — Pratt-style precedence climbing
// ------------------------------------------------------------------------
typedef enum {
    PREC_NONE,       //
    PREC_ASSIGN,     // = += -= *= /=
    PREC_OR,         // ||
    PREC_AND,        // &&
    PREC_EQUALITY,   // == !=
    PREC_COMPARISON, // < <= > >=
    PREC_TERM,       // + -
    PREC_FACTOR,     // * / %
    PREC_UNARY,      // ! -
    PREC_CALL,       // () .
    PREC_PRIMARY,    //
} Precedence;

static Precedence get_precedence(TokenKind kind) {
    switch (kind) {
    case TOK_EQ:
    case TOK_PLUS_EQ:
    case TOK_MINUS_EQ:
    case TOK_STAR_EQ:
    case TOK_SLASH_EQ:
        return PREC_ASSIGN;
    case TOK_PIPE_PIPE:
        return PREC_OR;
    case TOK_AMP_AMP:
        return PREC_AND;
    case TOK_EQ_EQ:
    case TOK_BANG_EQ:
        return PREC_EQUALITY;
    case TOK_LT:
    case TOK_LT_EQ:
    case TOK_GT:
    case TOK_GT_EQ:
        return PREC_COMPARISON;
    case TOK_PLUS:
    case TOK_MINUS:
        return PREC_TERM;
    case TOK_STAR:
    case TOK_SLASH:
    case TOK_PERCENT:
        return PREC_FACTOR;
    default:
        return PREC_NONE;
    }
}

static ASTNode *parse_str_interp(Parser *p, SrcLoc s) {
    ASTNode *interp = ast_new(p->arena, NODE_STR_INTERP, s);
    interp->str_interp.parts = NULL;

    ASTNode *text = ast_new(p->arena, NODE_LITERAL, s);
    text->literal.kind = LIT_STR;
    text->literal.str_val = previous(p)->lit.str_val;
    BUF_PUSH(interp->str_interp.parts, text);

    while (match(p, TOK_INTERP_START)) {
        ASTNode *expr = parse_expr(p);
        BUF_PUSH(interp->str_interp.parts, expr);
        expect(p, TOK_INTERP_END);

        SrcLoc ts = loc(p);
        expect(p, TOK_STR_LIT);
        ASTNode *text2 = ast_new(p->arena, NODE_LITERAL, ts);
        text2->literal.kind = LIT_STR;
        text2->literal.str_val = previous(p)->lit.str_val;
        BUF_PUSH(interp->str_interp.parts, text2);
    }
    return interp;
}

static ASTNode *parse_primary(Parser *p) {
    SrcLoc s = loc(p);

    if (match(p, TOK_INT_LIT)) {
        ASTNode *n = ast_new(p->arena, NODE_LITERAL, s);
        n->literal.kind = LIT_I32;
        n->literal.int_val = previous(p)->lit.int_val;
        return n;
    }
    if (match(p, TOK_FLOAT_LIT)) {
        ASTNode *n = ast_new(p->arena, NODE_LITERAL, s);
        n->literal.kind = LIT_F64;
        n->literal.f64_val = previous(p)->lit.float_val;
        return n;
    }
    if (match(p, TOK_STR_LIT)) {
        if (check(p, TOK_INTERP_START)) {
            return parse_str_interp(p, s);
        }
        ASTNode *n = ast_new(p->arena, NODE_LITERAL, s);
        n->literal.kind = LIT_STR;
        n->literal.str_val = previous(p)->lit.str_val;
        return n;
    }
    if (match(p, TOK_TRUE)) {
        ASTNode *n = ast_new(p->arena, NODE_LITERAL, s);
        n->literal.kind = LIT_BOOL;
        n->literal.bool_val = true;
        return n;
    }
    if (match(p, TOK_FALSE)) {
        ASTNode *n = ast_new(p->arena, NODE_LITERAL, s);
        n->literal.kind = LIT_BOOL;
        n->literal.bool_val = false;
        return n;
    }
    if (match(p, TOK_IDENT)) {
        ASTNode *n = ast_new(p->arena, NODE_IDENT, s);
        n->ident.name = previous(p)->lexeme;
        return n;
    }
    if (match(p, TOK_LPAREN)) {
        ASTNode *expr = parse_expr(p);
        expect(p, TOK_RPAREN);
        return expr;
    }
    if (check(p, TOK_IF)) {
        return parse_expr(p);
    }
    if (check(p, TOK_LBRACE)) {
        return parse_block(p);
    }
    rg_error(s, "expected expression, got '%s'", token_kind_str(current(p)->kind));
    advance_tok(p);
    return ast_new(p->arena, NODE_LITERAL, s); // error recovery
}

static ASTNode *parse_postfix(Parser *p) {
    ASTNode *left = parse_primary(p);
    for (;;) {
        SrcLoc s = loc(p);
        if (match(p, TOK_LPAREN)) {
            ASTNode *n = ast_new(p->arena, NODE_CALL, s);
            n->call.callee = left;
            n->call.args = NULL;
            if (!check(p, TOK_RPAREN)) {
                do {
                    skip_newlines(p);
                    BUF_PUSH(n->call.args, parse_expr(p));
                } while (match(p, TOK_COMMA));
            }
            expect(p, TOK_RPAREN);
            left = n;
            continue;
        }
        if (match(p, TOK_DOT)) {
            ASTNode *n = ast_new(p->arena, NODE_MEMBER, s);
            n->member.object = left;
            n->member.member = expect(p, TOK_IDENT)->lexeme;
            left = n;
            continue;
        }
        break;
    }
    return left;
}

static ASTNode *parse_unary(Parser *p) {
    if (check(p, TOK_MINUS) || check(p, TOK_BANG)) {
        SrcLoc s = loc(p);
        TokenKind op = advance_tok(p)->kind;
        ASTNode *n = ast_new(p->arena, NODE_UNARY, s);
        n->unary.op = op;
        n->unary.operand = parse_unary(p);
        return n;
    }
    return parse_postfix(p);
}

static ASTNode *parse_prec(Parser *p, Precedence min_prec) {
    ASTNode *left = parse_unary(p);

    for (;;) {
        TokenKind op = current(p)->kind;
        Precedence prec = get_precedence(op);
        if (prec < min_prec) {
            break;
        }

        SrcLoc s = loc(p);
        advance_tok(p); // consume operator

        // Assignment
        if (op == TOK_EQ) {
            ASTNode *n = ast_new(p->arena, NODE_ASSIGN, s);
            n->assign.target = left;
            n->assign.value = parse_prec(p, prec); // right-assoc
            left = n;
            continue;
        }

        // Compound assignment
        if (op == TOK_PLUS_EQ || op == TOK_MINUS_EQ || op == TOK_STAR_EQ || op == TOK_SLASH_EQ) {
            ASTNode *n = ast_new(p->arena, NODE_COMPOUND_ASSIGN, s);
            n->compound_assign.op = op;
            n->compound_assign.target = left;
            n->compound_assign.value = parse_prec(p, prec);
            left = n;
            continue;
        }

        // Binary
        ASTNode *n = ast_new(p->arena, NODE_BINARY, s);
        n->binary.op = op;
        n->binary.left = left;
        n->binary.right = parse_prec(p, prec + 1);
        left = n;
    }

    return left;
}

static ASTNode *parse_if(Parser *p) {
    SrcLoc s = loc(p);
    expect(p, TOK_IF);
    ASTNode *n = ast_new(p->arena, NODE_IF, s);
    n->if_expr.cond = parse_expr(p);
    n->if_expr.then_body = parse_block(p);
    n->if_expr.else_body = NULL;
    skip_newlines(p);
    if (match(p, TOK_ELSE)) {
        skip_newlines(p);
        if (check(p, TOK_IF)) {
            n->if_expr.else_body = parse_if(p);
        } else {
            n->if_expr.else_body = parse_block(p);
        }
    }
    return n;
}

static ASTNode *parse_expr(Parser *p) {
    if (check(p, TOK_IF)) {
        return parse_if(p);
    }
    return parse_prec(p, PREC_ASSIGN);
}

// ------------------------------------------------------------------------
// Blocks
// ------------------------------------------------------------------------
static ASTNode *parse_block(Parser *p) {
    SrcLoc s = loc(p);
    expect(p, TOK_LBRACE);
    skip_newlines(p);

    ASTNode *n = ast_new(p->arena, NODE_BLOCK, s);
    n->block.stmts = NULL;
    n->block.result = NULL;

    while (!check(p, TOK_RBRACE) && !at_end(p)) {
        ASTNode *stmt = parse_stmt(p);
        BUF_PUSH(n->block.stmts, stmt);
        skip_newlines(p);
    }

    // If the last statement is a bare expression, make it the block result
    int32_t n_stmts = BUF_LEN(n->block.stmts);
    if (n_stmts > 0) {
        ASTNode *last = n->block.stmts[n_stmts - 1];
        if (last->kind == NODE_EXPR_STMT) {
            n->block.result = last->expr_stmt.expr;
            BUF__HDR(n->block.stmts)->len--;
        }
    }

    expect(p, TOK_RBRACE);
    return n;
}

// ------------------------------------------------------------------------
// Statements and declarations
// ------------------------------------------------------------------------
static ASTNode *parse_var_decl(Parser *p) {
    SrcLoc s = loc(p);
    ASTNode *n = ast_new(p->arena, NODE_VAR_DECL, s);

    // `var x: T = expr` or `x := expr`
    if (match(p, TOK_VAR)) {
        n->var_decl.is_var = true;
        n->var_decl.name = expect(p, TOK_IDENT)->lexeme;
        n->var_decl.type.kind = AST_TYPE_INFERRED;

        if (match(p, TOK_COLON)) {
            n->var_decl.type = parse_type(p);
        }
        expect(p, TOK_EQ);
        n->var_decl.init = parse_expr(p);
    } else {
        // `name := expr` — already consumed IDENT, needs look-ahead
        n->var_decl.is_var = false;
        n->var_decl.name = previous(p)->lexeme;
        n->var_decl.type.kind = AST_TYPE_INFERRED;
        expect(p, TOK_COLON_EQ);
        n->var_decl.init = parse_expr(p);
    }

    return n;
}

static ASTNode *parse_fn_decl(Parser *p, bool is_pub) {
    SrcLoc s = loc(p);
    expect(p, TOK_FN);

    ASTNode *n = ast_new(p->arena, NODE_FN_DECL, s);
    n->fn_decl.is_pub = is_pub;
    n->fn_decl.name = expect(p, TOK_IDENT)->lexeme;
    n->fn_decl.params = NULL;

    // Parameters
    expect(p, TOK_LPAREN);
    if (!check(p, TOK_RPAREN)) {
        do {
            SrcLoc ps = loc(p);
            ASTNode *param = ast_new(p->arena, NODE_PARAM, ps);
            param->param.name = expect(p, TOK_IDENT)->lexeme;
            expect(p, TOK_COLON);
            param->param.type = parse_type(p);
            BUF_PUSH(n->fn_decl.params, param);
        } while (match(p, TOK_COMMA));
    }
    expect(p, TOK_RPAREN);

    // Return type
    n->fn_decl.return_type.kind = AST_TYPE_INFERRED;
    if (match(p, TOK_ARROW)) {
        n->fn_decl.return_type = parse_type(p);
    }

    skip_newlines(p);

    // Body: block or `= expr`
    if (check(p, TOK_LBRACE)) {
        n->fn_decl.body = parse_block(p);
    } else if (match(p, TOK_EQ)) {
        n->fn_decl.body = parse_expr(p);
    } else {
        rg_error(loc(p), "expected function body");
    }

    return n;
}

static ASTNode *parse_assert(Parser *p) {
    SrcLoc s = loc(p);
    expect(p, TOK_ASSERT);

    ASTNode *n = ast_new(p->arena, NODE_ASSERT, s);
    n->assert_stmt.cond = parse_expr(p);
    n->assert_stmt.message = NULL;

    if (match(p, TOK_COMMA)) {
        n->assert_stmt.message = parse_expr(p);
    }

    return n;
}

static ASTNode *parse_loop(Parser *p) {
    SrcLoc s = loc(p);
    expect(p, TOK_LOOP);
    ASTNode *n = ast_new(p->arena, NODE_LOOP, s);
    n->loop.body = parse_block(p);
    return n;
}

static ASTNode *parse_for(Parser *p) {
    SrcLoc s = loc(p);
    expect(p, TOK_FOR);

    ASTNode *n = ast_new(p->arena, NODE_FOR, s);
    n->for_loop.start = parse_expr(p);
    expect(p, TOK_DOT_DOT);
    n->for_loop.end = parse_expr(p);
    expect(p, TOK_PIPE);
    n->for_loop.var_name = expect(p, TOK_IDENT)->lexeme;
    expect(p, TOK_PIPE);
    n->for_loop.body = parse_block(p);
    return n;
}

static ASTNode *parse_stmt(Parser *p) {
    skip_newlines(p);

    // Keyword-initiated statements
    if (check(p, TOK_VAR)) {
        return parse_var_decl(p);
    }
    if (check(p, TOK_ASSERT)) {
        return parse_assert(p);
    }
    if (check(p, TOK_LOOP)) {
        return parse_loop(p);
    }
    if (check(p, TOK_FOR)) {
        return parse_for(p);
    }

    if (check(p, TOK_BREAK)) {
        SrcLoc s = loc(p);
        advance_tok(p); // consume 'break'
        return ast_new(p->arena, NODE_BREAK, s);
    }
    if (check(p, TOK_CONTINUE)) {
        SrcLoc s = loc(p);
        advance_tok(p); // consume 'continue'
        return ast_new(p->arena, NODE_CONTINUE, s);
    }

    // `ident :=` — inferred variable declaration
    if (check(p, TOK_IDENT) && p->pos + 1 < p->count && p->tokens[p->pos + 1].kind == TOK_COLON_EQ) {
        advance_tok(p); // consume IDENT
        return parse_var_decl(p);
    }

    // Expression statement
    SrcLoc s = loc(p);
    ASTNode *expr = parse_expr(p);
    ASTNode *n = ast_new(p->arena, NODE_EXPR_STMT, s);
    n->expr_stmt.expr = expr;
    return n;
}

static ASTNode *parse_decl(Parser *p) {
    skip_newlines(p);

    // module
    if (check(p, TOK_MODULE)) {
        SrcLoc s = loc(p);
        advance_tok(p); // consume 'module'
        ASTNode *n = ast_new(p->arena, NODE_MODULE, s);
        n->module.name = expect(p, TOK_IDENT)->lexeme;
        return n;
    }

    // pub fn ...
    if (check(p, TOK_PUB)) {
        advance_tok(p); // consume 'pub'
        skip_newlines(p);
        if (check(p, TOK_FN)) {
            return parse_fn_decl(p, true);
        }
        rg_error(loc(p), "expected 'fn' after 'pub'");
        return NULL;
    }

    // fn ...
    if (check(p, TOK_FN)) {
        return parse_fn_decl(p, false);
    }

    // Top-level statement (for scripts)
    return parse_stmt(p);
}

// ------------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------------
Parser *parser_create(const Token *tokens, int32_t count, Arena *arena, const char *file) {
    Parser *p = malloc(sizeof(*p));
    if (p == NULL) {
        rg_fatal("out of memory");
    }
    p->tokens = tokens;
    p->pos = 0;
    p->count = count;
    p->arena = arena;
    p->file = file;
    return p;
}

void parser_destroy(Parser *p) {
    free(p);
}

ASTNode *parser_parse(Parser *p) {
    SrcLoc s = {.file = p->file, .line = 1, .col = 1};
    ASTNode *file = ast_new(p->arena, NODE_FILE, s);
    file->file.decls = NULL;

    skip_newlines(p);
    while (!at_end(p)) {
        ASTNode *decl = parse_decl(p);
        if (decl != NULL) {
            BUF_PUSH(file->file.decls, decl);
        }
        skip_newlines(p);
    }

    return file;
}
