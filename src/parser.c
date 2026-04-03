#include "parser.h"

struct Parser {
    const Token *tokens; /* buf */
    int32_t position;
    int32_t count;
    Arena *arena;     // AST allocation arena
    const char *file; // source filename for diagnostics
};

// Token-stream navigation helpers.
static const Token *current(const Parser *parser) {
    return &parser->tokens[parser->position];
}

static const Token *previous(const Parser *parser) {
    return &parser->tokens[parser->position - 1];
}

static bool at_end(const Parser *parser) {
    return current(parser)->kind == TOKEN_EOF;
}

static bool check(const Parser *parser, TokenKind kind) {
    return current(parser)->kind == kind;
}

static const Token *advance_token(Parser *parser) {
    if (!at_end(parser)) {
        parser->position++;
    }
    return previous(parser);
}

static bool match(Parser *parser, TokenKind kind) {
    if (check(parser, kind)) {
        advance_token(parser);
        return true;
    }
    return false;
}

/**
 * Consume the current token if it matches @p kind; otherwise emit an error
 * and return the current token without advancing.
 */
static const Token *expect(Parser *parser, TokenKind kind) {
    if (check(parser, kind)) {
        return advance_token(parser);
    }
    rsg_error(current(parser)->location, "expected '%s', got '%s'", token_kind_string(kind),
              token_kind_string(current(parser)->kind));
    return current(parser);
}

static void skip_newlines(Parser *parser) {
    while (check(parser, TOKEN_NEWLINE)) {
        advance_token(parser);
    }
}

static SourceLocation current_location(const Parser *parser) {
    return current(parser)->location;
}

// Forward declarations for mutually-recursive descent.
static ASTNode *parse_expression(Parser *parser);
static ASTNode *parse_statement(Parser *parser);
static ASTNode *parse_block(Parser *parser);
static ASTNode *parse_declaration(Parser *parser);

/** Parse an explicit type annotation (e.g. i32, str, bool, [N]T, (A,B)). */
static ASTType parse_type(Parser *parser) {
    ASTType type = {.kind = AST_TYPE_NAME, .location = current_location(parser)};

    // Array type: [N]T
    if (check(parser, TOKEN_LEFT_BRACKET)) {
        type.kind = AST_TYPE_ARRAY;
        advance_token(parser); // consume '['
        if (!check(parser, TOKEN_INTEGER_LITERAL)) {
            rsg_error(current_location(parser), "expected array size");
            type.kind = AST_TYPE_INFERRED;
            return type;
        }
        type.array_size = (int32_t)current(parser)->literal_value.integer_value;
        advance_token(parser); // consume size
        expect(parser, TOKEN_RIGHT_BRACKET);
        ASTType *element = arena_alloc(parser->arena, sizeof(ASTType));
        *element = parse_type(parser);
        type.array_element = element;
        return type;
    }

    // Tuple type: (T, U, ...)
    if (check(parser, TOKEN_LEFT_PAREN)) {
        type.kind = AST_TYPE_TUPLE;
        type.tuple_elements = NULL;
        advance_token(parser); // consume '('
        if (!check(parser, TOKEN_RIGHT_PAREN)) {
            do {
                ASTType *element = arena_alloc(parser->arena, sizeof(ASTType));
                *element = parse_type(parser);
                BUFFER_PUSH(type.tuple_elements, element);
            } while (match(parser, TOKEN_COMMA));
        }
        expect(parser, TOKEN_RIGHT_PAREN);
        return type;
    }

    if (token_is_type_keyword(current(parser)->kind) || check(parser, TOKEN_IDENTIFIER)) {
        type.name = advance_token(parser)->lexeme;
    } else {
        rsg_error(current_location(parser), "expected type name");
        type.kind = AST_TYPE_INFERRED;
    }
    return type;
}

/** Operator precedence levels for Pratt-style parsing. */
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
    case TOKEN_EQUAL:
    case TOKEN_PLUS_EQUAL:
    case TOKEN_MINUS_EQUAL:
    case TOKEN_STAR_EQUAL:
    case TOKEN_SLASH_EQUAL:
        return PREC_ASSIGN;
    case TOKEN_PIPE_PIPE:
        return PREC_OR;
    case TOKEN_AMPERSAND_AMPERSAND:
        return PREC_AND;
    case TOKEN_EQUAL_EQUAL:
    case TOKEN_BANG_EQUAL:
        return PREC_EQUALITY;
    case TOKEN_LESS:
    case TOKEN_LESS_EQUAL:
    case TOKEN_GREATER:
    case TOKEN_GREATER_EQUAL:
        return PREC_COMPARISON;
    case TOKEN_PLUS:
    case TOKEN_MINUS:
        return PREC_TERM;
    case TOKEN_STAR:
    case TOKEN_SLASH:
    case TOKEN_PERCENT:
        return PREC_FACTOR;
    default:
        return PREC_NONE;
    }
}

static ASTNode *parse_string_interpolation(Parser *parser, SourceLocation location) {
    ASTNode *interpolation = ast_new(parser->arena, NODE_STRING_INTERPOLATION, location);
    interpolation->string_interpolation.parts = NULL;

    ASTNode *text = ast_new(parser->arena, NODE_LITERAL, location);
    text->literal.kind = LITERAL_STRING;
    text->literal.string_value = previous(parser)->literal_value.string_value;
    BUFFER_PUSH(interpolation->string_interpolation.parts, text);

    while (match(parser, TOKEN_INTERPOLATION_START)) {
        ASTNode *expression = parse_expression(parser);
        BUFFER_PUSH(interpolation->string_interpolation.parts, expression);
        expect(parser, TOKEN_INTERPOLATION_END);

        SourceLocation text_location = current_location(parser);
        expect(parser, TOKEN_STRING_LITERAL);
        ASTNode *text2 = ast_new(parser->arena, NODE_LITERAL, text_location);
        text2->literal.kind = LITERAL_STRING;
        text2->literal.string_value = previous(parser)->literal_value.string_value;
        BUFFER_PUSH(interpolation->string_interpolation.parts, text2);
    }
    return interpolation;
}

/**
 * Parse an array literal: [expr, ...] or [N]T[expr, ...].
 * The opening '[' has NOT been consumed yet.
 */
static ASTNode *parse_array_literal(Parser *parser) {
    SourceLocation location = current_location(parser);
    expect(parser, TOKEN_LEFT_BRACKET);

    // Check if this is [N]T[values] (typed array literal)
    // Pattern: INTEGER_LITERAL, ']', type-keyword-or-identifier
    if (check(parser, TOKEN_INTEGER_LITERAL) && parser->position + 1 < parser->count &&
        parser->tokens[parser->position + 1].kind == TOKEN_RIGHT_BRACKET && parser->position + 2 < parser->count &&
        (token_is_type_keyword(parser->tokens[parser->position + 2].kind) ||
         parser->tokens[parser->position + 2].kind == TOKEN_IDENTIFIER)) {
        int32_t size = (int32_t)current(parser)->literal_value.integer_value;
        advance_token(parser); // consume size
        advance_token(parser); // consume ']'

        // Now parse element type
        ASTNode *node = ast_new(parser->arena, NODE_ARRAY_LITERAL, location);
        node->array_literal.element_type = parse_type(parser);
        node->array_literal.size = size;
        node->array_literal.elements = NULL;

        // Parse [values]
        expect(parser, TOKEN_LEFT_BRACKET);
        if (!check(parser, TOKEN_RIGHT_BRACKET)) {
            do {
                skip_newlines(parser);
                BUFFER_PUSH(node->array_literal.elements, parse_expression(parser));
            } while (match(parser, TOKEN_COMMA));
        }
        expect(parser, TOKEN_RIGHT_BRACKET);
        return node;
    }

    // Simple array literal: [expr, expr, ...]
    ASTNode *node = ast_new(parser->arena, NODE_ARRAY_LITERAL, location);
    node->array_literal.element_type.kind = AST_TYPE_INFERRED;
    node->array_literal.elements = NULL;
    if (!check(parser, TOKEN_RIGHT_BRACKET)) {
        do {
            skip_newlines(parser);
            BUFFER_PUSH(node->array_literal.elements, parse_expression(parser));
        } while (match(parser, TOKEN_COMMA));
    }
    node->array_literal.size = BUFFER_LENGTH(node->array_literal.elements);
    expect(parser, TOKEN_RIGHT_BRACKET);
    return node;
}

static ASTNode *parse_primary(Parser *parser) {
    SourceLocation location = current_location(parser);

    if (match(parser, TOKEN_INTEGER_LITERAL)) {
        ASTNode *node = ast_new(parser->arena, NODE_LITERAL, location);
        node->literal.kind = LITERAL_I32;
        node->literal.integer_value = previous(parser)->literal_value.integer_value;
        return node;
    }
    if (match(parser, TOKEN_FLOAT_LITERAL)) {
        ASTNode *node = ast_new(parser->arena, NODE_LITERAL, location);
        node->literal.kind = LITERAL_F64;
        node->literal.float64_value = previous(parser)->literal_value.float_value;
        return node;
    }
    if (match(parser, TOKEN_CHAR_LITERAL)) {
        ASTNode *node = ast_new(parser->arena, NODE_LITERAL, location);
        node->literal.kind = LITERAL_CHAR;
        node->literal.char_value = previous(parser)->literal_value.char_value;
        return node;
    }
    if (match(parser, TOKEN_STRING_LITERAL)) {
        if (check(parser, TOKEN_INTERPOLATION_START)) {
            return parse_string_interpolation(parser, location);
        }
        ASTNode *node = ast_new(parser->arena, NODE_LITERAL, location);
        node->literal.kind = LITERAL_STRING;
        node->literal.string_value = previous(parser)->literal_value.string_value;
        return node;
    }
    if (match(parser, TOKEN_TRUE)) {
        ASTNode *node = ast_new(parser->arena, NODE_LITERAL, location);
        node->literal.kind = LITERAL_BOOL;
        node->literal.boolean_value = true;
        return node;
    }
    if (match(parser, TOKEN_FALSE)) {
        ASTNode *node = ast_new(parser->arena, NODE_LITERAL, location);
        node->literal.kind = LITERAL_BOOL;
        node->literal.boolean_value = false;
        return node;
    }

    // Typed literal syntax: type_keyword(expr) e.g. i64(100), f32(3.14)
    if (token_is_type_keyword(current(parser)->kind) && parser->position + 1 < parser->count &&
        parser->tokens[parser->position + 1].kind == TOKEN_LEFT_PAREN) {
        ASTNode *node = ast_new(parser->arena, NODE_TYPE_CONVERSION, location);
        node->type_conversion.target_type.kind = AST_TYPE_NAME;
        node->type_conversion.target_type.name = advance_token(parser)->lexeme;
        node->type_conversion.target_type.location = location;
        expect(parser, TOKEN_LEFT_PAREN);
        node->type_conversion.operand = parse_expression(parser);
        expect(parser, TOKEN_RIGHT_PAREN);
        return node;
    }

    if (match(parser, TOKEN_IDENTIFIER)) {
        ASTNode *node = ast_new(parser->arena, NODE_IDENTIFIER, location);
        node->identifier.name = previous(parser)->lexeme;
        return node;
    }

    // Array literal: [N]T[elems] (typed) or handled via var decl context
    if (check(parser, TOKEN_LEFT_BRACKET)) {
        return parse_array_literal(parser);
    }

    // Parenthesized expression or tuple literal
    if (match(parser, TOKEN_LEFT_PAREN)) {
        ASTNode *first = parse_expression(parser);
        if (match(parser, TOKEN_COMMA)) {
            // Tuple literal: (expr, expr, ...)
            ASTNode *node = ast_new(parser->arena, NODE_TUPLE_LITERAL, location);
            node->tuple_literal.elements = NULL;
            BUFFER_PUSH(node->tuple_literal.elements, first);
            do {
                skip_newlines(parser);
                BUFFER_PUSH(node->tuple_literal.elements, parse_expression(parser));
            } while (match(parser, TOKEN_COMMA));
            expect(parser, TOKEN_RIGHT_PAREN);
            return node;
        }
        expect(parser, TOKEN_RIGHT_PAREN);
        return first;
    }

    if (check(parser, TOKEN_IF)) {
        return parse_expression(parser);
    }
    if (check(parser, TOKEN_LEFT_BRACE)) {
        return parse_block(parser);
    }
    rsg_error(location, "expected expression, got '%s'", token_kind_string(current(parser)->kind));
    advance_token(parser);
    return ast_new(parser->arena, NODE_LITERAL, location); // error recovery
}

static ASTNode *parse_postfix(Parser *parser) {
    ASTNode *left = parse_primary(parser);
    for (;;) {
        SourceLocation location = current_location(parser);
        if (match(parser, TOKEN_LEFT_PAREN)) {
            ASTNode *node = ast_new(parser->arena, NODE_CALL, location);
            node->call.callee = left;
            node->call.arguments = NULL;
            if (!check(parser, TOKEN_RIGHT_PAREN)) {
                do {
                    skip_newlines(parser);
                    BUFFER_PUSH(node->call.arguments, parse_expression(parser));
                } while (match(parser, TOKEN_COMMA));
            }
            expect(parser, TOKEN_RIGHT_PAREN);
            left = node;
            continue;
        }
        if (match(parser, TOKEN_LEFT_BRACKET)) {
            ASTNode *node = ast_new(parser->arena, NODE_INDEX, location);
            node->index_access.object = left;
            node->index_access.index = parse_expression(parser);
            expect(parser, TOKEN_RIGHT_BRACKET);
            left = node;
            continue;
        }
        if (match(parser, TOKEN_DOT)) {
            // Tuple member access: t.0, t.1, or struct/module access: t.name
            if (check(parser, TOKEN_INTEGER_LITERAL)) {
                ASTNode *node = ast_new(parser->arena, NODE_MEMBER, location);
                node->member.object = left;
                node->member.member = arena_sprintf(parser->arena, "%llu",
                                                    (unsigned long long)current(parser)->literal_value.integer_value);
                advance_token(parser);
                left = node;
            } else {
                ASTNode *node = ast_new(parser->arena, NODE_MEMBER, location);
                node->member.object = left;
                node->member.member = expect(parser, TOKEN_IDENTIFIER)->lexeme;
                left = node;
            }
            continue;
        }
        break;
    }
    return left;
}

static ASTNode *parse_unary(Parser *parser) {
    if (check(parser, TOKEN_MINUS) || check(parser, TOKEN_BANG)) {
        SourceLocation location = current_location(parser);
        TokenKind op = advance_token(parser)->kind;
        ASTNode *node = ast_new(parser->arena, NODE_UNARY, location);
        node->unary.op = op;
        node->unary.operand = parse_unary(parser);
        return node;
    }
    return parse_postfix(parser);
}

static ASTNode *parse_precedence(Parser *parser, Precedence minimum_precedence) {
    ASTNode *left = parse_unary(parser);

    for (;;) {
        TokenKind op = current(parser)->kind;
        Precedence precedence = get_precedence(op);
        if (precedence < minimum_precedence) {
            break;
        }

        SourceLocation location = current_location(parser);
        advance_token(parser); // consume operator

        // Assignment
        if (op == TOKEN_EQUAL) {
            ASTNode *node = ast_new(parser->arena, NODE_ASSIGN, location);
            node->assign.target = left;
            node->assign.value = parse_precedence(parser, precedence); // right-assoc
            left = node;
            continue;
        }

        // Compound assignment
        if (op == TOKEN_PLUS_EQUAL || op == TOKEN_MINUS_EQUAL || op == TOKEN_STAR_EQUAL || op == TOKEN_SLASH_EQUAL) {
            ASTNode *node = ast_new(parser->arena, NODE_COMPOUND_ASSIGN, location);
            node->compound_assign.op = op;
            node->compound_assign.target = left;
            node->compound_assign.value = parse_precedence(parser, precedence);
            left = node;
            continue;
        }

        // Binary
        ASTNode *node = ast_new(parser->arena, NODE_BINARY, location);
        node->binary.op = op;
        node->binary.left = left;
        node->binary.right = parse_precedence(parser, precedence + 1);
        left = node;
    }

    return left;
}

static ASTNode *parse_if(Parser *parser) {
    SourceLocation location = current_location(parser);
    expect(parser, TOKEN_IF);
    ASTNode *node = ast_new(parser->arena, NODE_IF, location);
    node->if_expression.condition = parse_expression(parser);
    node->if_expression.then_body = parse_block(parser);
    node->if_expression.else_body = NULL;
    skip_newlines(parser);
    if (match(parser, TOKEN_ELSE)) {
        skip_newlines(parser);
        if (check(parser, TOKEN_IF)) {
            node->if_expression.else_body = parse_if(parser);
        } else {
            node->if_expression.else_body = parse_block(parser);
        }
    }
    return node;
}

static ASTNode *parse_expression(Parser *parser) {
    if (check(parser, TOKEN_IF)) {
        return parse_if(parser);
    }
    return parse_precedence(parser, PREC_ASSIGN);
}

/**
 * Parse a brace-enclosed block: @c { stmts... [trailing_expr] }.
 * The last bare expression (if any) becomes the block's result value.
 */
static ASTNode *parse_block(Parser *parser) {
    SourceLocation location = current_location(parser);
    expect(parser, TOKEN_LEFT_BRACE);
    skip_newlines(parser);

    ASTNode *node = ast_new(parser->arena, NODE_BLOCK, location);
    node->block.statements = NULL;
    node->block.result = NULL;

    while (!check(parser, TOKEN_RIGHT_BRACE) && !at_end(parser)) {
        ASTNode *statement = parse_statement(parser);
        BUFFER_PUSH(node->block.statements, statement);
        skip_newlines(parser);
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

    expect(parser, TOKEN_RIGHT_BRACE);
    return node;
}

// Statement and declaration parsers.

/**
 * Parse `var x: T = expr` or the second half of `name := expr` (IDENT
 * already consumed).
 */
static ASTNode *parse_variable_declaration(Parser *parser) {
    SourceLocation location = current_location(parser);
    ASTNode *node = ast_new(parser->arena, NODE_VARIABLE_DECLARATION, location);

    // `var x: T = expr` or `x := expr`
    if (match(parser, TOKEN_VARIABLE)) {
        node->variable_declaration.is_variable = true;
        node->variable_declaration.name = expect(parser, TOKEN_IDENTIFIER)->lexeme;
        node->variable_declaration.type.kind = AST_TYPE_INFERRED;

        if (match(parser, TOKEN_COLON)) {
            node->variable_declaration.type = parse_type(parser);
        }
        expect(parser, TOKEN_EQUAL);
        node->variable_declaration.initializer = parse_expression(parser);
    } else {
        // `name := expr` - already consumed IDENT, needs look-ahead
        node->variable_declaration.is_variable = false;
        node->variable_declaration.name = previous(parser)->lexeme;
        node->variable_declaration.type.kind = AST_TYPE_INFERRED;
        expect(parser, TOKEN_COLON_EQUAL);
        node->variable_declaration.initializer = parse_expression(parser);
    }

    return node;
}

static ASTNode *parse_function_declaration(Parser *parser, bool is_public) {
    SourceLocation location = current_location(parser);
    expect(parser, TOKEN_FUNCTION);

    ASTNode *node = ast_new(parser->arena, NODE_FUNCTION_DECLARATION, location);
    node->function_declaration.is_public = is_public;
    node->function_declaration.name = expect(parser, TOKEN_IDENTIFIER)->lexeme;
    node->function_declaration.parameters = NULL;

    // Parameters
    expect(parser, TOKEN_LEFT_PAREN);
    if (!check(parser, TOKEN_RIGHT_PAREN)) {
        do {
            SourceLocation parameter_location = current_location(parser);
            ASTNode *parameter = ast_new(parser->arena, NODE_PARAMETER, parameter_location);
            parameter->parameter.name = expect(parser, TOKEN_IDENTIFIER)->lexeme;
            expect(parser, TOKEN_COLON);
            parameter->parameter.type = parse_type(parser);
            BUFFER_PUSH(node->function_declaration.parameters, parameter);
        } while (match(parser, TOKEN_COMMA));
    }
    expect(parser, TOKEN_RIGHT_PAREN);

    // Return type
    node->function_declaration.return_type.kind = AST_TYPE_INFERRED;
    if (match(parser, TOKEN_ARROW)) {
        node->function_declaration.return_type = parse_type(parser);
    }

    skip_newlines(parser);

    // Body: block or `= expr`
    if (check(parser, TOKEN_LEFT_BRACE)) {
        node->function_declaration.body = parse_block(parser);
    } else if (match(parser, TOKEN_EQUAL)) {
        node->function_declaration.body = parse_expression(parser);
    } else {
        rsg_error(current_location(parser), "expected function body");
    }

    return node;
}

static ASTNode *parse_loop(Parser *parser) {
    SourceLocation location = current_location(parser);
    expect(parser, TOKEN_LOOP);
    ASTNode *node = ast_new(parser->arena, NODE_LOOP, location);
    node->loop.body = parse_block(parser);
    return node;
}

static ASTNode *parse_for(Parser *parser) {
    SourceLocation location = current_location(parser);
    expect(parser, TOKEN_FOR);

    ASTNode *node = ast_new(parser->arena, NODE_FOR, location);
    node->for_loop.start = parse_expression(parser);
    expect(parser, TOKEN_DOT_DOT);
    node->for_loop.end = parse_expression(parser);
    expect(parser, TOKEN_PIPE);
    node->for_loop.variable_name = expect(parser, TOKEN_IDENTIFIER)->lexeme;
    expect(parser, TOKEN_PIPE);
    node->for_loop.body = parse_block(parser);
    return node;
}

static ASTNode *parse_statement(Parser *parser) {
    skip_newlines(parser);

    // Keyword-initiated statements
    if (check(parser, TOKEN_VARIABLE)) {
        return parse_variable_declaration(parser);
    }
    if (check(parser, TOKEN_LOOP)) {
        return parse_loop(parser);
    }
    if (check(parser, TOKEN_FOR)) {
        return parse_for(parser);
    }

    if (check(parser, TOKEN_BREAK)) {
        SourceLocation location = current_location(parser);
        advance_token(parser); // consume 'break'
        return ast_new(parser->arena, NODE_BREAK, location);
    }
    if (check(parser, TOKEN_CONTINUE)) {
        SourceLocation location = current_location(parser);
        advance_token(parser); // consume 'continue'
        return ast_new(parser->arena, NODE_CONTINUE, location);
    }

    // `ident :=` - inferred variable declaration
    bool is_inferred_declaration = check(parser, TOKEN_IDENTIFIER) && parser->position + 1 < parser->count &&
                                   parser->tokens[parser->position + 1].kind == TOKEN_COLON_EQUAL;
    if (is_inferred_declaration) {
        advance_token(parser); // consume IDENT
        return parse_variable_declaration(parser);
    }

    // Expression statement
    SourceLocation location = current_location(parser);
    ASTNode *expression = parse_expression(parser);
    ASTNode *node = ast_new(parser->arena, NODE_EXPRESSION_STATEMENT, location);
    node->expression_statement.expression = expression;
    return node;
}

static ASTNode *parse_declaration(Parser *parser) {
    skip_newlines(parser);

    // module
    if (check(parser, TOKEN_MODULE)) {
        SourceLocation location = current_location(parser);
        advance_token(parser); // consume 'module'
        ASTNode *node = ast_new(parser->arena, NODE_MODULE, location);
        node->module.name = expect(parser, TOKEN_IDENTIFIER)->lexeme;
        return node;
    }

    // type alias: type Name = UnderlyingType
    if (check(parser, TOKEN_TYPE)) {
        SourceLocation location = current_location(parser);
        advance_token(parser); // consume 'type'
        ASTNode *node = ast_new(parser->arena, NODE_TYPE_ALIAS, location);
        node->type_alias.name = expect(parser, TOKEN_IDENTIFIER)->lexeme;
        expect(parser, TOKEN_EQUAL);
        node->type_alias.alias_type = parse_type(parser);
        return node;
    }

    // pub fn ...
    if (check(parser, TOKEN_PUBLIC)) {
        advance_token(parser); // consume 'pub'
        skip_newlines(parser);
        if (check(parser, TOKEN_FUNCTION)) {
            return parse_function_declaration(parser, true);
        }
        rsg_error(current_location(parser), "expected 'fn' after 'pub'");
        return NULL;
    }

    // fn ...
    if (check(parser, TOKEN_FUNCTION)) {
        return parse_function_declaration(parser, false);
    }

    // Top-level statement (for scripts)
    return parse_statement(parser);
}

Parser *parser_create(const Token *tokens, int32_t count, Arena *arena, const char *file) {
    Parser *parser = malloc(sizeof(*parser));
    if (parser == NULL) {
        rsg_fatal("out of memory");
    }
    parser->tokens = tokens;
    parser->position = 0;
    parser->count = count;
    parser->arena = arena;
    parser->file = file;
    return parser;
}

void parser_destroy(Parser *parser) {
    free(parser);
}

ASTNode *parser_parse(Parser *parser) {
    SourceLocation location = {.file = parser->file, .line = 1, .column = 1};
    ASTNode *file = ast_new(parser->arena, NODE_FILE, location);
    file->file.declarations = NULL;

    skip_newlines(parser);
    while (!at_end(parser)) {
        ASTNode *declaration = parse_declaration(parser);
        if (declaration != NULL) {
            BUFFER_PUSH(file->file.declarations, declaration);
        }
        skip_newlines(parser);
    }

    return file;
}
