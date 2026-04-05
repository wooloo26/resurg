#include "_parser.h"

// ── Variable declaration ───────────────────────────────────────────────

/**
 * Parse `var x: T = expr` or the second half of `name := expr` (IDENT
 * already consumed).
 */
static ASTNode *parse_variable_declaration(Parser *parser) {
    SourceLocation location = parser_current_location(parser);
    ASTNode *node = ast_new(parser->arena, NODE_VARIABLE_DECLARATION, location);
    node->variable_declaration.is_immut = false;

    // `immut x := expr`
    if (parser_match(parser, TOKEN_IMMUT)) {
        node->variable_declaration.is_immut = true;
        node->variable_declaration.is_variable = false;
        node->variable_declaration.name = parser_expect(parser, TOKEN_IDENTIFIER)->lexeme;
        node->variable_declaration.type.kind = AST_TYPE_INFERRED;
        parser_expect(parser, TOKEN_COLON_EQUAL);
        node->variable_declaration.initializer = parser_parse_expression(parser);
        return node;
    }

    // `var x: T = expr` or `x := expr`
    if (parser_match(parser, TOKEN_VARIABLE)) {
        node->variable_declaration.is_variable = true;
        node->variable_declaration.name = parser_expect(parser, TOKEN_IDENTIFIER)->lexeme;
        node->variable_declaration.type.kind = AST_TYPE_INFERRED;

        if (parser_match(parser, TOKEN_COLON)) {
            node->variable_declaration.type = parser_parse_type(parser);
        }
        parser_expect(parser, TOKEN_EQUAL);
        node->variable_declaration.initializer = parser_parse_expression(parser);
    } else {
        // `name := expr` - already consumed IDENT, needs look-ahead
        node->variable_declaration.is_variable = false;
        node->variable_declaration.name = parser_previous_token(parser)->lexeme;
        node->variable_declaration.type.kind = AST_TYPE_INFERRED;
        parser_expect(parser, TOKEN_COLON_EQUAL);
        node->variable_declaration.initializer = parser_parse_expression(parser);
    }

    return node;
}

// ── Loop constructs ────────────────────────────────────────────────────

static ASTNode *parse_loop(Parser *parser) {
    SourceLocation location = parser_current_location(parser);
    parser_expect(parser, TOKEN_LOOP);
    ASTNode *node = ast_new(parser->arena, NODE_LOOP, location);
    node->loop.body = parser_parse_block(parser);
    return node;
}

static ASTNode *parse_for(Parser *parser) {
    SourceLocation location = parser_current_location(parser);
    parser_expect(parser, TOKEN_FOR);

    ASTNode *node = ast_new(parser->arena, NODE_FOR, location);
    node->for_loop.start = parser_parse_expression(parser);
    parser_expect(parser, TOKEN_DOT_DOT);
    node->for_loop.end = parser_parse_expression(parser);
    parser_expect(parser, TOKEN_PIPE);
    node->for_loop.variable_name = parser_expect(parser, TOKEN_IDENTIFIER)->lexeme;
    parser_expect(parser, TOKEN_PIPE);
    node->for_loop.body = parser_parse_block(parser);
    return node;
}

// ── Block ──────────────────────────────────────────────────────────────

ASTNode *parser_parse_block(Parser *parser) {
    SourceLocation location = parser_current_location(parser);
    parser_expect(parser, TOKEN_LEFT_BRACE);
    parser_skip_newlines(parser);

    ASTNode *node = ast_new(parser->arena, NODE_BLOCK, location);
    node->block.statements = NULL;
    node->block.result = NULL;

    while (!parser_check(parser, TOKEN_RIGHT_BRACE) && !parser_at_end(parser)) {
        ASTNode *statement = parser_parse_statement(parser);
        BUFFER_PUSH(node->block.statements, statement);
        parser_skip_newlines(parser);
    }

    // If the last statement is a bare expression, make it the block result
    int32_t statement_count = BUFFER_LENGTH(node->block.statements);
    if (statement_count > 0) {
        ASTNode *last = node->block.statements[statement_count - 1];
        if (last->kind == NODE_EXPRESSION_STATEMENT) {
            node->block.result = last->expression_statement.expression;
            BUFFER__HEADER(node->block.statements)->length--;
        }
    }

    parser_expect(parser, TOKEN_RIGHT_BRACE);
    return node;
}

// ── Statement dispatch ─────────────────────────────────────────────────

/** Return true if current position looks like struct destructuring:
 *  { IDENT [: IDENT] (, IDENT [: IDENT])* } := */
static bool is_struct_destructure(const Parser *parser) {
    if (!parser_check(parser, TOKEN_LEFT_BRACE)) {
        return false;
    }
    int32_t pos = parser->position + 1;
    while (pos < parser->count) {
        // Skip newlines
        while (pos < parser->count && parser->tokens[pos].kind == TOKEN_NEWLINE) {
            pos++;
        }
        if (pos >= parser->count || parser->tokens[pos].kind != TOKEN_IDENTIFIER) {
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
            if (pos >= parser->count || parser->tokens[pos].kind != TOKEN_IDENTIFIER) {
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

/** Return true if current position looks like tuple destructuring: ( IDENT|.. (, IDENT|..)* ) := */
static bool is_tuple_destructure(const Parser *parser) {
    if (!parser_check(parser, TOKEN_LEFT_PAREN)) {
        return false;
    }
    int32_t pos = parser->position + 1;
    int32_t name_count = 0;
    while (pos < parser->count) {
        if (parser->tokens[pos].kind == TOKEN_DOT_DOT ||
            parser->tokens[pos].kind == TOKEN_IDENTIFIER) {
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
            continue;
        }
        return false;
    }
    return false;
}

static ASTNode *parse_struct_destructure(Parser *parser) {
    SourceLocation location = parser_current_location(parser);
    parser_expect(parser, TOKEN_LEFT_BRACE);

    ASTNode *node = ast_new(parser->arena, NODE_STRUCT_DESTRUCTURE, location);
    node->struct_destructure.field_names = NULL;
    node->struct_destructure.aliases = NULL;

    do {
        parser_skip_newlines(parser);
        const char *name = parser_expect(parser, TOKEN_IDENTIFIER)->lexeme;
        BUFFER_PUSH(node->struct_destructure.field_names, name);
        // Optional alias: `name: alias`
        if (parser_match(parser, TOKEN_COLON)) {
            const char *alias = parser_expect(parser, TOKEN_IDENTIFIER)->lexeme;
            BUFFER_PUSH(node->struct_destructure.aliases, alias);
        } else {
            BUFFER_PUSH(node->struct_destructure.aliases, (const char *)NULL);
        }
    } while (parser_match(parser, TOKEN_COMMA));

    parser_skip_newlines(parser);
    parser_expect(parser, TOKEN_RIGHT_BRACE);
    parser_expect(parser, TOKEN_COLON_EQUAL);
    node->struct_destructure.value = parser_parse_expression(parser);
    return node;
}

static ASTNode *parse_tuple_destructure(Parser *parser) {
    SourceLocation location = parser_current_location(parser);
    parser_expect(parser, TOKEN_LEFT_PAREN);

    ASTNode *node = ast_new(parser->arena, NODE_TUPLE_DESTRUCTURE, location);
    node->tuple_destructure.names = NULL;
    node->tuple_destructure.has_rest = false;
    node->tuple_destructure.rest_position = -1;

    int32_t index = 0;
    do {
        if (parser_match(parser, TOKEN_DOT_DOT)) {
            if (node->tuple_destructure.has_rest) {
                rsg_error(parser_current_location(parser),
                          "only one '..' allowed in tuple destructure");
            }
            node->tuple_destructure.has_rest = true;
            node->tuple_destructure.rest_position = index;
        } else {
            const char *name = parser_expect(parser, TOKEN_IDENTIFIER)->lexeme;
            BUFFER_PUSH(node->tuple_destructure.names, name);
            index++;
        }
    } while (parser_match(parser, TOKEN_COMMA));

    parser_expect(parser, TOKEN_RIGHT_PAREN);
    parser_expect(parser, TOKEN_COLON_EQUAL);
    node->tuple_destructure.value = parser_parse_expression(parser);
    return node;
}

ASTNode *parser_parse_statement(Parser *parser) {
    parser_skip_newlines(parser);

    // Keyword-initiated statements
    if (parser_check(parser, TOKEN_VARIABLE)) {
        return parse_variable_declaration(parser);
    }
    if (parser_check(parser, TOKEN_IMMUT)) {
        return parse_variable_declaration(parser);
    }
    if (parser_check(parser, TOKEN_LOOP)) {
        return parse_loop(parser);
    }
    if (parser_check(parser, TOKEN_FOR)) {
        return parse_for(parser);
    }

    if (parser_check(parser, TOKEN_BREAK)) {
        SourceLocation location = parser_current_location(parser);
        parser_advance(parser); // consume 'break'
        return ast_new(parser->arena, NODE_BREAK, location);
    }
    if (parser_check(parser, TOKEN_CONTINUE)) {
        SourceLocation location = parser_current_location(parser);
        parser_advance(parser); // consume 'continue'
        return ast_new(parser->arena, NODE_CONTINUE, location);
    }

    // Struct destructuring: {a, b} := expr
    if (is_struct_destructure(parser)) {
        return parse_struct_destructure(parser);
    }

    // Tuple destructuring: (a, b) := expr
    if (is_tuple_destructure(parser)) {
        return parse_tuple_destructure(parser);
    }

    // `ident :=` - inferred variable declaration
    bool is_inferred_declaration = parser_check(parser, TOKEN_IDENTIFIER) &&
                                   parser->position + 1 < parser->count &&
                                   parser->tokens[parser->position + 1].kind == TOKEN_COLON_EQUAL;
    if (is_inferred_declaration) {
        parser_advance(parser); // consume IDENT
        return parse_variable_declaration(parser);
    }

    // Expression statement
    SourceLocation location = parser_current_location(parser);
    ASTNode *expression = parser_parse_expression(parser);
    ASTNode *node = ast_new(parser->arena, NODE_EXPRESSION_STATEMENT, location);
    node->expression_statement.expression = expression;
    return node;
}
