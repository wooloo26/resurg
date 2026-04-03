#include "parser/_parser.h"

// ── Function declaration ───────────────────────────────────────────────

static ASTNode *parse_function_declaration(Parser *parser, bool is_public) {
    SourceLocation location = parser_current_location(parser);
    parser_expect(parser, TOKEN_FUNCTION);

    ASTNode *node = ast_new(parser->arena, NODE_FUNCTION_DECLARATION, location);
    node->function_declaration.is_public = is_public;
    node->function_declaration.name = parser_expect(parser, TOKEN_IDENTIFIER)->lexeme;
    node->function_declaration.parameters = NULL;

    // Parameters
    parser_expect(parser, TOKEN_LEFT_PAREN);
    if (!parser_check(parser, TOKEN_RIGHT_PAREN)) {
        do {
            SourceLocation parameter_location = parser_current_location(parser);
            ASTNode *parameter = ast_new(parser->arena, NODE_PARAMETER, parameter_location);
            parameter->parameter.name = parser_expect(parser, TOKEN_IDENTIFIER)->lexeme;
            parser_expect(parser, TOKEN_COLON);
            parameter->parameter.type = parser_parse_type(parser);
            BUFFER_PUSH(node->function_declaration.parameters, parameter);
        } while (parser_match(parser, TOKEN_COMMA));
    }
    parser_expect(parser, TOKEN_RIGHT_PAREN);

    // Return type
    node->function_declaration.return_type.kind = AST_TYPE_INFERRED;
    if (parser_match(parser, TOKEN_ARROW)) {
        node->function_declaration.return_type = parser_parse_type(parser);
    }

    parser_skip_newlines(parser);

    // Body: block or `= expr`
    if (parser_check(parser, TOKEN_LEFT_BRACE)) {
        node->function_declaration.body = parser_parse_block(parser);
    } else if (parser_match(parser, TOKEN_EQUAL)) {
        node->function_declaration.body = parser_parse_expression(parser);
    } else {
        rsg_error(parser_current_location(parser), "expected function body");
    }

    return node;
}

// ── Top-level declaration dispatch ─────────────────────────────────────

ASTNode *parser_parse_declaration(Parser *parser) {
    parser_skip_newlines(parser);

    // module
    if (parser_check(parser, TOKEN_MODULE)) {
        SourceLocation location = parser_current_location(parser);
        parser_advance(parser); // consume 'module'
        ASTNode *node = ast_new(parser->arena, NODE_MODULE, location);
        node->module.name = parser_expect(parser, TOKEN_IDENTIFIER)->lexeme;
        return node;
    }

    // type alias: type Name = UnderlyingType
    if (parser_check(parser, TOKEN_TYPE)) {
        SourceLocation location = parser_current_location(parser);
        parser_advance(parser); // consume 'type'
        ASTNode *node = ast_new(parser->arena, NODE_TYPE_ALIAS, location);
        node->type_alias.name = parser_expect(parser, TOKEN_IDENTIFIER)->lexeme;
        parser_expect(parser, TOKEN_EQUAL);
        node->type_alias.alias_type = parser_parse_type(parser);
        return node;
    }

    // pub fn ...
    if (parser_check(parser, TOKEN_PUBLIC)) {
        parser_advance(parser); // consume 'pub'
        parser_skip_newlines(parser);
        if (parser_check(parser, TOKEN_FUNCTION)) {
            return parse_function_declaration(parser, true);
        }
        rsg_error(parser_current_location(parser), "expected 'fn' after 'pub'");
        return NULL;
    }

    // fn ...
    if (parser_check(parser, TOKEN_FUNCTION)) {
        return parse_function_declaration(parser, false);
    }

    // Top-level statement (for scripts)
    return parser_parse_statement(parser);
}
