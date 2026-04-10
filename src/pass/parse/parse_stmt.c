#include "_parse.h"

// ── Var decl ───────────────────────────────────────────────

/**
 * Parse `var x: T = expr` or the second half of `name := expr` (IDENT
 * already consumed).
 */
static ASTNode *parse_var_decl(Parser *parser) {
    SrcLoc loc = parser_current_loc(parser);
    ASTNode *node = ast_new(parser->arena, NODE_VAR_DECL, loc);
    node->var_decl.is_immut = false;

    // `immut x := expr`
    if (parser_match(parser, TOKEN_IMMUT)) {
        node->var_decl.is_immut = true;
        node->var_decl.is_var = false;
        node->var_decl.name = parser_expect(parser, TOKEN_ID)->lexeme;
        node->var_decl.type.kind = AST_TYPE_INFERRED;
        parser_expect(parser, TOKEN_COLON_EQUAL);
        node->var_decl.init = parser_parse_expr(parser);
        return node;
    }

    // `var x: T = expr` or `x := expr` or `var x` (declare first)
    if (parser_match(parser, TOKEN_VAR)) {
        node->var_decl.is_var = true;
        node->var_decl.name = parser_expect(parser, TOKEN_ID)->lexeme;
        node->var_decl.type.kind = AST_TYPE_INFERRED;

        if (parser_match(parser, TOKEN_COLON)) {
            node->var_decl.type = parser_parse_type(parser);
        }
        if (parser_match(parser, TOKEN_EQUAL)) {
            node->var_decl.init = parser_parse_expr(parser);
        } else {
            node->var_decl.init = NULL;
        }
    } else {
        // `name := expr` - already consumed IDENT, needs look-ahead
        node->var_decl.is_var = false;
        node->var_decl.name = parser_previous_token(parser)->lexeme;
        node->var_decl.type.kind = AST_TYPE_INFERRED;
        parser_expect(parser, TOKEN_COLON_EQUAL);
        node->var_decl.init = parser_parse_expr(parser);
    }

    return node;
}

// ── Loop constructs ────────────────────────────────────────────────────

static ASTNode *parse_loop(Parser *parser) {
    SrcLoc loc = parser_current_loc(parser);
    parser_expect(parser, TOKEN_LOOP);
    ASTNode *node = ast_new(parser->arena, NODE_LOOP, loc);
    node->loop.body = parser_parse_block(parser);
    return node;
}

static ASTNode *parse_while(Parser *parser) {
    SrcLoc loc = parser_current_loc(parser);
    parser_expect(parser, TOKEN_WHILE);
    ASTNode *node = ast_new(parser->arena, NODE_WHILE, loc);

    bool saved = parser->no_struct_lit;
    parser->no_struct_lit = true;

    if (parser_is_pattern_binding(parser)) {
        node->while_loop.pattern = parser_parse_pattern(parser);
        parser_expect(parser, TOKEN_COLON_EQUAL);
        node->while_loop.pattern_init = parser_parse_expr(parser);
        node->while_loop.cond = NULL;
    } else {
        node->while_loop.cond = parser_parse_expr(parser);
        node->while_loop.pattern = NULL;
        node->while_loop.pattern_init = NULL;
    }

    parser->no_struct_lit = saved;

    node->while_loop.body = parser_parse_block(parser);
    return node;
}

static ASTNode *parse_defer(Parser *parser) {
    SrcLoc loc = parser_current_loc(parser);
    parser_expect(parser, TOKEN_DEFER);
    ASTNode *node = ast_new(parser->arena, NODE_DEFER, loc);
    if (parser_check(parser, TOKEN_LEFT_BRACE)) {
        node->defer_stmt.body = parser_parse_block(parser);
    } else {
        // Single-statement defer: defer expr
        node->defer_stmt.body = parser_parse_stmt(parser);
    }
    return node;
}

static ASTNode *parse_for(Parser *parser) {
    SrcLoc loc = parser_current_loc(parser);
    parser_expect(parser, TOKEN_FOR);

    ASTNode *node = ast_new(parser->arena, NODE_FOR, loc);
    node->for_loop.idx_name = NULL;
    node->for_loop.iterable = NULL;
    node->for_loop.start = NULL;
    node->for_loop.end = NULL;

    // Parse the first expr (could be range start or iterable)
    ASTNode *first = parser_parse_expr(parser);

    if (parser_check(parser, TOKEN_DOT_DOT)) {
        // Range form: for start..end |i| { body }
        parser_advance(parser); // consume '..'
        node->for_loop.start = first;
        node->for_loop.end = parser_parse_expr(parser);
        parser_expect(parser, TOKEN_PIPE);
        node->for_loop.var_name = parser_expect(parser, TOKEN_ID)->lexeme;
        parser_expect(parser, TOKEN_PIPE);
    } else {
        // Slice form: for slice |v| { body } or for slice |v, i| { body }
        node->for_loop.iterable = first;
        parser_expect(parser, TOKEN_PIPE);
        node->for_loop.var_name = parser_expect(parser, TOKEN_ID)->lexeme;
        if (parser_match(parser, TOKEN_COMMA)) {
            node->for_loop.idx_name = parser_expect(parser, TOKEN_ID)->lexeme;
        }
        parser_expect(parser, TOKEN_PIPE);
    }

    node->for_loop.body = parser_parse_block(parser);
    return node;
}

// ── Block ──────────────────────────────────────────────────────────────

ASTNode *parser_parse_block(Parser *parser) {
    SrcLoc loc = parser_current_loc(parser);
    parser_expect(parser, TOKEN_LEFT_BRACE);
    parser_skip_newlines(parser);

    ASTNode *node = ast_new(parser->arena, NODE_BLOCK, loc);
    node->block.stmts = NULL;
    node->block.result = NULL;

    while (!parser_check(parser, TOKEN_RIGHT_BRACE) && !parser_at_end(parser)) {
        ASTNode *stmt = parser_parse_stmt(parser);
        BUF_PUSH(node->block.stmts, stmt);
        parser_skip_newlines(parser);
    }

    // If the last stmt is a bare expr, make it the block result
    int32_t stmt_count = BUF_LEN(node->block.stmts);
    if (stmt_count > 0) {
        ASTNode *last = node->block.stmts[stmt_count - 1];
        if (last->kind == NODE_EXPR_STMT) {
            node->block.result = last->expr_stmt.expr;
            BUF__HEADER(node->block.stmts)->len--;
        }
    }

    parser_expect(parser, TOKEN_RIGHT_BRACE);
    return node;
}

// ── Statement dispatch ─────────────────────────────────────────────────

/** Return true if current pos looks like struct destructuring:
 *  { IDENT [: IDENT] (, IDENT [: IDENT])* } := */
static bool is_struct_destructure(const Parser *parser) {
    if (!parser_check(parser, TOKEN_LEFT_BRACE)) {
        return false;
    }
    int32_t pos = parser->pos + 1;
    while (pos < parser->count) {
        // Skip newlines
        while (pos < parser->count && parser->tokens[pos].kind == TOKEN_NEWLINE) {
            pos++;
        }
        if (pos >= parser->count || parser->tokens[pos].kind != TOKEN_ID) {
            return false;
        }
        pos++;
        // Skip newlines
        while (pos < parser->count && parser->tokens[pos].kind == TOKEN_NEWLINE) {
            pos++;
        }
        if (pos >= parser->count) {
            return false;
        }
        // Optional alias: `: IDENT`
        if (parser->tokens[pos].kind == TOKEN_COLON) {
            pos++;
            // Skip newlines
            while (pos < parser->count && parser->tokens[pos].kind == TOKEN_NEWLINE) {
                pos++;
            }
            if (pos >= parser->count || parser->tokens[pos].kind != TOKEN_ID) {
                return false;
            }
            pos++;
            // Skip newlines
            while (pos < parser->count && parser->tokens[pos].kind == TOKEN_NEWLINE) {
                pos++;
            }
            if (pos >= parser->count) {
                return false;
            }
        }
        if (parser->tokens[pos].kind == TOKEN_RIGHT_BRACE) {
            return pos + 1 < parser->count && parser->tokens[pos + 1].kind == TOKEN_COLON_EQUAL;
        }
        if (parser->tokens[pos].kind == TOKEN_COMMA) {
            pos++;
            continue;
        }
        return false;
    }
    return false;
}

/** Return true if current pos looks like tuple destructuring: ( IDENT|.. (, IDENT|..)* ) := */
static bool is_tuple_destructure(const Parser *parser) {
    if (!parser_check(parser, TOKEN_LEFT_PAREN)) {
        return false;
    }
    int32_t pos = parser->pos + 1;
    int32_t name_count = 0;
    while (pos < parser->count) {
        if (parser->tokens[pos].kind == TOKEN_DOT_DOT || parser->tokens[pos].kind == TOKEN_ID) {
            pos++;
            name_count++;
        } else {
            return false;
        }
        if (pos >= parser->count) {
            return false;
        }
        if (parser->tokens[pos].kind == TOKEN_RIGHT_PAREN) {
            return name_count >= 1 && pos + 1 < parser->count &&
                   parser->tokens[pos + 1].kind == TOKEN_COLON_EQUAL;
        }
        if (parser->tokens[pos].kind == TOKEN_COMMA) {
            pos++;
            // Trailing comma: (x,) :=
            if (pos < parser->count && parser->tokens[pos].kind == TOKEN_RIGHT_PAREN) {
                return name_count >= 1 && pos + 1 < parser->count &&
                       parser->tokens[pos + 1].kind == TOKEN_COLON_EQUAL;
            }
            continue;
        }
        return false;
    }
    return false;
}

static ASTNode *parse_struct_destructure(Parser *parser) {
    SrcLoc loc = parser_current_loc(parser);
    parser_expect(parser, TOKEN_LEFT_BRACE);

    ASTNode *node = ast_new(parser->arena, NODE_STRUCT_DESTRUCTURE, loc);
    node->struct_destructure.field_names = NULL;
    node->struct_destructure.aliases = NULL;

    do {
        parser_skip_newlines(parser);
        const char *name = parser_expect(parser, TOKEN_ID)->lexeme;
        BUF_PUSH(node->struct_destructure.field_names, name);
        // Optional alias: `name: alias`
        if (parser_match(parser, TOKEN_COLON)) {
            const char *alias = parser_expect(parser, TOKEN_ID)->lexeme;
            BUF_PUSH(node->struct_destructure.aliases, alias);
        } else {
            BUF_PUSH(node->struct_destructure.aliases, (const char *)NULL);
        }
    } while (parser_match(parser, TOKEN_COMMA));

    parser_skip_newlines(parser);
    parser_expect(parser, TOKEN_RIGHT_BRACE);
    parser_expect(parser, TOKEN_COLON_EQUAL);
    node->struct_destructure.value = parser_parse_expr(parser);
    return node;
}

static ASTNode *parse_tuple_destructure(Parser *parser) {
    SrcLoc loc = parser_current_loc(parser);
    parser_expect(parser, TOKEN_LEFT_PAREN);

    ASTNode *node = ast_new(parser->arena, NODE_TUPLE_DESTRUCTURE, loc);
    node->tuple_destructure.names = NULL;
    node->tuple_destructure.has_rest = false;
    node->tuple_destructure.rest_pos = -1;

    int32_t idx = 0;
    do {
        if (parser_match(parser, TOKEN_DOT_DOT)) {
            if (node->tuple_destructure.has_rest) {
                PARSER_ERR(parser, parser_current_loc(parser),
                           "only one '..' allowed in tuple destructure");
            }
            node->tuple_destructure.has_rest = true;
            node->tuple_destructure.rest_pos = idx;
        } else {
            const char *name = parser_expect(parser, TOKEN_ID)->lexeme;
            BUF_PUSH(node->tuple_destructure.names, name);
            idx++;
        }
    } while (parser_match(parser, TOKEN_COMMA) && !parser_check(parser, TOKEN_RIGHT_PAREN));

    parser_expect(parser, TOKEN_RIGHT_PAREN);
    parser_expect(parser, TOKEN_COLON_EQUAL);
    node->tuple_destructure.value = parser_parse_expr(parser);
    return node;
}

ASTNode *parser_parse_stmt(Parser *parser) {
    parser_skip_newlines(parser);

    // Keyword-initiated stmts
    if (parser_check(parser, TOKEN_VAR)) {
        return parse_var_decl(parser);
    }
    if (parser_check(parser, TOKEN_IMMUT)) {
        return parse_var_decl(parser);
    }
    if (parser_check(parser, TOKEN_LOOP)) {
        return parse_loop(parser);
    }
    if (parser_check(parser, TOKEN_WHILE)) {
        return parse_while(parser);
    }
    if (parser_check(parser, TOKEN_FOR)) {
        return parse_for(parser);
    }
    if (parser_check(parser, TOKEN_DEFER)) {
        return parse_defer(parser);
    }

    if (parser_check(parser, TOKEN_BREAK)) {
        SrcLoc loc = parser_current_loc(parser);
        parser_advance(parser); // consume 'break'
        ASTNode *node = ast_new(parser->arena, NODE_BREAK, loc);
        node->break_stmt.value = NULL;
        if (!parser_check(parser, TOKEN_NEWLINE) && !parser_check(parser, TOKEN_SEMICOLON) &&
            !parser_check(parser, TOKEN_RIGHT_BRACE) && !parser_at_end(parser)) {
            node->break_stmt.value = parser_parse_expr(parser);
        }
        return node;
    }
    if (parser_check(parser, TOKEN_CONTINUE)) {
        SrcLoc loc = parser_current_loc(parser);
        parser_advance(parser); // consume 'continue'
        return ast_new(parser->arena, NODE_CONTINUE, loc);
    }

    if (parser_check(parser, TOKEN_RETURN)) {
        SrcLoc loc = parser_current_loc(parser);
        parser_advance(parser); // consume 'return'
        ASTNode *node = ast_new(parser->arena, NODE_RETURN, loc);
        node->return_stmt.value = NULL;
        if (!parser_check(parser, TOKEN_NEWLINE) && !parser_check(parser, TOKEN_SEMICOLON) &&
            !parser_check(parser, TOKEN_RIGHT_BRACE) && !parser_at_end(parser)) {
            node->return_stmt.value = parser_parse_expr(parser);
        }
        return node;
    }

    // Struct destructuring: {a, b} := expr
    if (is_struct_destructure(parser)) {
        return parse_struct_destructure(parser);
    }

    // Tuple destructuring: (a, b) := expr
    if (is_tuple_destructure(parser)) {
        return parse_tuple_destructure(parser);
    }

    // `ident :=` - inferred var decl
    bool is_inferred_decl = parser_check(parser, TOKEN_ID) && parser->pos + 1 < parser->count &&
                            parser->tokens[parser->pos + 1].kind == TOKEN_COLON_EQUAL;
    if (is_inferred_decl) {
        parser_advance(parser); // consume IDENT
        return parse_var_decl(parser);
    }

    // Expression stmt
    SrcLoc loc = parser_current_loc(parser);
    ASTNode *expr = parser_parse_expr(parser);
    ASTNode *node = ast_new(parser->arena, NODE_EXPR_STMT, loc);
    node->expr_stmt.expr = expr;
    return node;
}
