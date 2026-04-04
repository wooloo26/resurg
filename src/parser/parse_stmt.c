#include "_parser.h"

// ── Variable declaration ───────────────────────────────────────────────

/**
 * Parse `var x: T = expr` or the second half of `name := expr` (IDENT
 * already consumed).
 */
static ASTNode *parse_variable_declaration(Parser *parser) {
    SourceLocation location = parser_current_location(parser);
    ASTNode *node = ast_new(parser->arena, NODE_VARIABLE_DECLARATION, location);

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

ASTNode *parser_parse_statement(Parser *parser) {
    parser_skip_newlines(parser);

    // Keyword-initiated statements
    if (parser_check(parser, TOKEN_VARIABLE)) {
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
