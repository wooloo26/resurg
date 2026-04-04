#include "_parser.h"

// ── Operator precedence ────────────────────────────────────────────────

/** Operator precedence levels for Pratt-style parsing. */
typedef enum {
    PRECEDENCE_NONE,       //
    PRECEDENCE_ASSIGN,     // = += -= *= /=
    PRECEDENCE_OR,         // ||
    PRECEDENCE_AND,        // &&
    PRECEDENCE_EQUALITY,   // == !=
    PRECEDENCE_COMPARISON, // < <= > >=
    PRECEDENCE_TERM,       // + -
    PRECEDENCE_FACTOR,     // * / %
    PRECEDENCE_UNARY,      // ! -
    PRECEDENCE_CALL,       // () .
    PRECEDENCE_PRIMARY,    //
} Precedence;

static Precedence get_precedence(TokenKind kind) {
    switch (kind) {
    case TOKEN_EQUAL:
    case TOKEN_PLUS_EQUAL:
    case TOKEN_MINUS_EQUAL:
    case TOKEN_STAR_EQUAL:
    case TOKEN_SLASH_EQUAL:
        return PRECEDENCE_ASSIGN;
    case TOKEN_PIPE_PIPE:
        return PRECEDENCE_OR;
    case TOKEN_AMPERSAND_AMPERSAND:
        return PRECEDENCE_AND;
    case TOKEN_EQUAL_EQUAL:
    case TOKEN_BANG_EQUAL:
        return PRECEDENCE_EQUALITY;
    case TOKEN_LESS:
    case TOKEN_LESS_EQUAL:
    case TOKEN_GREATER:
    case TOKEN_GREATER_EQUAL:
        return PRECEDENCE_COMPARISON;
    case TOKEN_PLUS:
    case TOKEN_MINUS:
        return PRECEDENCE_TERM;
    case TOKEN_STAR:
    case TOKEN_SLASH:
    case TOKEN_PERCENT:
        return PRECEDENCE_FACTOR;
    default:
        return PRECEDENCE_NONE;
    }
}

// ── Leaf / primary parsers ────────────────────────────────────────────

static ASTNode *parse_string_interpolation(Parser *parser, SourceLocation location) {
    ASTNode *interpolation = ast_new(parser->arena, NODE_STRING_INTERPOLATION, location);
    interpolation->string_interpolation.parts = NULL;

    ASTNode *text = ast_new(parser->arena, NODE_LITERAL, location);
    text->literal.kind = LITERAL_STRING;
    text->literal.string_value = parser_previous_token(parser)->literal_value.string_value;
    BUFFER_PUSH(interpolation->string_interpolation.parts, text);

    while (parser_match(parser, TOKEN_INTERPOLATION_START)) {
        ASTNode *expression = parser_parse_expression(parser);
        BUFFER_PUSH(interpolation->string_interpolation.parts, expression);
        parser_expect(parser, TOKEN_INTERPOLATION_END);

        SourceLocation text_location = parser_current_location(parser);
        parser_expect(parser, TOKEN_STRING_LITERAL);
        ASTNode *text2 = ast_new(parser->arena, NODE_LITERAL, text_location);
        text2->literal.kind = LITERAL_STRING;
        text2->literal.string_value = parser_previous_token(parser)->literal_value.string_value;
        BUFFER_PUSH(interpolation->string_interpolation.parts, text2);
    }
    return interpolation;
}

/**
 * Parse a comma-separated list of expressions, appending each to @p buf.
 * Assumes the list has at least one element.  Skips newlines between items.
 */
static void parse_comma_separated(Parser *parser, ASTNode ***buf) {
    do {
        parser_skip_newlines(parser);
        BUFFER_PUSH(*buf, parser_parse_expression(parser));
    } while (parser_match(parser, TOKEN_COMMA));
}

/**
 * Parse a struct literal body: { field = expr, ... }.
 * The struct name identifier has already been consumed and passed as @p name_node.
 */
static ASTNode *parse_struct_literal(Parser *parser, ASTNode *name_node) {
    SourceLocation location = name_node->location;
    parser_expect(parser, TOKEN_LEFT_BRACE);
    parser_skip_newlines(parser);

    ASTNode *node = ast_new(parser->arena, NODE_STRUCT_LITERAL, location);
    node->struct_literal.name = name_node->identifier.name;
    node->struct_literal.field_names = NULL;
    node->struct_literal.field_values = NULL;

    if (!parser_check(parser, TOKEN_RIGHT_BRACE)) {
        do {
            parser_skip_newlines(parser);
            const char *field_name = parser_expect(parser, TOKEN_IDENTIFIER)->lexeme;
            parser_expect(parser, TOKEN_EQUAL);
            ASTNode *value = parser_parse_expression(parser);
            BUFFER_PUSH(node->struct_literal.field_names, field_name);
            BUFFER_PUSH(node->struct_literal.field_values, value);
        } while (parser_match(parser, TOKEN_COMMA));
    }
    parser_skip_newlines(parser);
    parser_expect(parser, TOKEN_RIGHT_BRACE);
    return node;
}

/** Return true if current position looks like a struct literal: IDENT { }  or IDENT { IDENT = ... }
 */
static bool is_struct_literal_ahead(const Parser *parser) {
    if (!parser_check(parser, TOKEN_LEFT_BRACE)) {
        return false;
    }
    int32_t pos = parser->position + 1;
    // Skip newlines
    while (pos < parser->count && parser->tokens[pos].kind == TOKEN_NEWLINE) {
        pos++;
    }
    if (pos >= parser->count) {
        return false;
    }
    // Empty struct literal: { }
    if (parser->tokens[pos].kind == TOKEN_RIGHT_BRACE) {
        return true;
    }
    // Named field: IDENT =
    if (parser->tokens[pos].kind == TOKEN_IDENTIFIER && pos + 1 < parser->count) {
        // Skip newlines after IDENT
        int32_t eq_pos = pos + 1;
        while (eq_pos < parser->count && parser->tokens[eq_pos].kind == TOKEN_NEWLINE) {
            eq_pos++;
        }
        if (eq_pos < parser->count && parser->tokens[eq_pos].kind == TOKEN_EQUAL) {
            return true;
        }
    }
    return false;
}

/**
 * Parse an array literal: [expr, ...] or [N]T[expr, ...].
 * The opening '[' has NOT been consumed yet.
 */
static ASTNode *parse_array_literal(Parser *parser) {
    SourceLocation location = parser_current_location(parser);
    parser_expect(parser, TOKEN_LEFT_BRACKET);

    // Check if this is [N]T[values] (typed array literal)
    // Pattern: INTEGER_LITERAL, ']', type-keyword-or-identifier
    bool has_size_and_bracket = parser_check(parser, TOKEN_INTEGER_LITERAL) &&
                                parser->position + 1 < parser->count &&
                                parser->tokens[parser->position + 1].kind == TOKEN_RIGHT_BRACKET;
    bool has_type_after = has_size_and_bracket && parser->position + 2 < parser->count &&
                          (token_is_type_keyword(parser->tokens[parser->position + 2].kind) ||
                           parser->tokens[parser->position + 2].kind == TOKEN_IDENTIFIER);
    if (has_type_after) {
        int32_t size = (int32_t)parser_current_token(parser)->literal_value.integer_value;
        parser_advance(parser); // consume size
        parser_advance(parser); // consume ']'

        // Now parse element type
        ASTNode *node = ast_new(parser->arena, NODE_ARRAY_LITERAL, location);
        node->array_literal.element_type = parser_parse_type(parser);
        node->array_literal.size = size;
        node->array_literal.elements = NULL;

        // Parse [values]
        parser_expect(parser, TOKEN_LEFT_BRACKET);
        if (!parser_check(parser, TOKEN_RIGHT_BRACKET)) {
            parse_comma_separated(parser, &node->array_literal.elements);
        }
        parser_expect(parser, TOKEN_RIGHT_BRACKET);
        return node;
    }

    // Simple array literal: [expr, expr, ...]
    ASTNode *node = ast_new(parser->arena, NODE_ARRAY_LITERAL, location);
    node->array_literal.element_type.kind = AST_TYPE_INFERRED;
    node->array_literal.elements = NULL;
    if (!parser_check(parser, TOKEN_RIGHT_BRACKET)) {
        parse_comma_separated(parser, &node->array_literal.elements);
    }
    node->array_literal.size = BUFFER_LENGTH(node->array_literal.elements);
    parser_expect(parser, TOKEN_RIGHT_BRACKET);
    return node;
}

static ASTNode *parse_primary(Parser *parser) {
    SourceLocation location = parser_current_location(parser);

    if (parser_match(parser, TOKEN_INTEGER_LITERAL)) {
        ASTNode *node = ast_new(parser->arena, NODE_LITERAL, location);
        node->literal.kind = LITERAL_I32;
        node->literal.integer_value = parser_previous_token(parser)->literal_value.integer_value;
        return node;
    }
    if (parser_match(parser, TOKEN_FLOAT_LITERAL)) {
        ASTNode *node = ast_new(parser->arena, NODE_LITERAL, location);
        node->literal.kind = LITERAL_F64;
        node->literal.float64_value = parser_previous_token(parser)->literal_value.float_value;
        return node;
    }
    if (parser_match(parser, TOKEN_CHAR_LITERAL)) {
        ASTNode *node = ast_new(parser->arena, NODE_LITERAL, location);
        node->literal.kind = LITERAL_CHAR;
        node->literal.char_value = parser_previous_token(parser)->literal_value.char_value;
        return node;
    }
    if (parser_match(parser, TOKEN_STRING_LITERAL)) {
        if (parser_check(parser, TOKEN_INTERPOLATION_START)) {
            return parse_string_interpolation(parser, location);
        }
        ASTNode *node = ast_new(parser->arena, NODE_LITERAL, location);
        node->literal.kind = LITERAL_STRING;
        node->literal.string_value = parser_previous_token(parser)->literal_value.string_value;
        return node;
    }
    if (parser_match(parser, TOKEN_TRUE)) {
        ASTNode *node = ast_new(parser->arena, NODE_LITERAL, location);
        node->literal.kind = LITERAL_BOOL;
        node->literal.boolean_value = true;
        return node;
    }
    if (parser_match(parser, TOKEN_FALSE)) {
        ASTNode *node = ast_new(parser->arena, NODE_LITERAL, location);
        node->literal.kind = LITERAL_BOOL;
        node->literal.boolean_value = false;
        return node;
    }

    // Typed literal syntax: type_keyword(expr) e.g. i64(100), f32(3.14)
    if (token_is_type_keyword(parser_current_token(parser)->kind) &&
        parser->position + 1 < parser->count &&
        parser->tokens[parser->position + 1].kind == TOKEN_LEFT_PAREN) {
        ASTNode *node = ast_new(parser->arena, NODE_TYPE_CONVERSION, location);
        node->type_conversion.target_type.kind = AST_TYPE_NAME;
        node->type_conversion.target_type.name = parser_advance(parser)->lexeme;
        node->type_conversion.target_type.location = location;
        parser_expect(parser, TOKEN_LEFT_PAREN);
        node->type_conversion.operand = parser_parse_expression(parser);
        parser_expect(parser, TOKEN_RIGHT_PAREN);
        return node;
    }

    if (parser_match(parser, TOKEN_IDENTIFIER)) {
        ASTNode *node = ast_new(parser->arena, NODE_IDENTIFIER, location);
        node->identifier.name = parser_previous_token(parser)->lexeme;
        return node;
    }

    // Array literal: [N]T[elems] (typed) or handled via var decl context
    if (parser_check(parser, TOKEN_LEFT_BRACKET)) {
        return parse_array_literal(parser);
    }

    // Parenthesized expression or tuple literal
    if (parser_match(parser, TOKEN_LEFT_PAREN)) {
        ASTNode *first = parser_parse_expression(parser);
        if (parser_match(parser, TOKEN_COMMA)) {
            // Tuple literal: (expr, expr, ...)
            ASTNode *node = ast_new(parser->arena, NODE_TUPLE_LITERAL, location);
            node->tuple_literal.elements = NULL;
            BUFFER_PUSH(node->tuple_literal.elements, first);
            parse_comma_separated(parser, &node->tuple_literal.elements);
            parser_expect(parser, TOKEN_RIGHT_PAREN);
            return node;
        }
        parser_expect(parser, TOKEN_RIGHT_PAREN);
        return first;
    }

    if (parser_check(parser, TOKEN_IF)) {
        return parser_parse_expression(parser);
    }
    if (parser_check(parser, TOKEN_LEFT_BRACE)) {
        return parser_parse_block(parser);
    }
    const char *token_name = token_kind_string(parser_current_token(parser)->kind);
    rsg_error(location, "expected expression, got '%s'", token_name);
    parser_advance(parser);
    return ast_new(parser->arena, NODE_LITERAL, location); // error recovery
}

// ── Postfix / unary / precedence climbing ──────────────────────────────

static ASTNode *parse_postfix(Parser *parser) {
    ASTNode *left = parse_primary(parser);
    for (;;) {
        SourceLocation location = parser_current_location(parser);

        // Struct literal: Identifier { field = expr, ... }
        if (left->kind == NODE_IDENTIFIER && is_struct_literal_ahead(parser)) {
            left = parse_struct_literal(parser, left);
            continue;
        }

        if (parser_match(parser, TOKEN_LEFT_PAREN)) {
            ASTNode *node = ast_new(parser->arena, NODE_CALL, location);
            node->call.callee = left;
            node->call.arguments = NULL;
            node->call.arg_names = NULL;
            if (!parser_check(parser, TOKEN_RIGHT_PAREN)) {
                do {
                    parser_skip_newlines(parser);
                    // Check for named argument: IDENT =
                    bool is_named_arg = parser_check(parser, TOKEN_IDENTIFIER) &&
                                        parser->position + 1 < parser->count &&
                                        parser->tokens[parser->position + 1].kind == TOKEN_EQUAL;
                    if (is_named_arg) {
                        const char *arg_name = parser_advance(parser)->lexeme;
                        parser_advance(parser); // consume '='
                        ASTNode *value = parser_parse_expression(parser);
                        BUFFER_PUSH(node->call.arguments, value);
                        BUFFER_PUSH(node->call.arg_names, arg_name);
                    } else {
                        ASTNode *arg = parser_parse_expression(parser);
                        BUFFER_PUSH(node->call.arguments, arg);
                        BUFFER_PUSH(node->call.arg_names, (const char *)NULL);
                    }
                } while (parser_match(parser, TOKEN_COMMA));
            }
            parser_expect(parser, TOKEN_RIGHT_PAREN);
            left = node;
            continue;
        }
        if (parser_match(parser, TOKEN_LEFT_BRACKET)) {
            ASTNode *node = ast_new(parser->arena, NODE_INDEX, location);
            node->index_access.object = left;
            node->index_access.index = parser_parse_expression(parser);
            parser_expect(parser, TOKEN_RIGHT_BRACKET);
            left = node;
            continue;
        }
        if (parser_match(parser, TOKEN_DOT)) {
            // Tuple member access: t.0, t.1, or struct/module access: t.name
            if (parser_check(parser, TOKEN_INTEGER_LITERAL)) {
                ASTNode *node = ast_new(parser->arena, NODE_MEMBER, location);
                node->member.object = left;
                unsigned long long index_value =
                    (unsigned long long)parser_current_token(parser)->literal_value.integer_value;
                node->member.member = arena_sprintf(parser->arena, "%llu", index_value);
                parser_advance(parser);
                left = node;
            } else {
                ASTNode *node = ast_new(parser->arena, NODE_MEMBER, location);
                node->member.object = left;
                node->member.member = parser_expect(parser, TOKEN_IDENTIFIER)->lexeme;
                left = node;
            }
            continue;
        }
        break;
    }
    return left;
}

static ASTNode *parse_unary(Parser *parser) {
    if (parser_check(parser, TOKEN_MINUS) || parser_check(parser, TOKEN_BANG)) {
        SourceLocation location = parser_current_location(parser);
        TokenKind op = parser_advance(parser)->kind;
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
        TokenKind op = parser_current_token(parser)->kind;
        Precedence precedence = get_precedence(op);
        if (precedence < minimum_precedence) {
            break;
        }

        SourceLocation location = parser_current_location(parser);
        parser_advance(parser); // consume operator

        // Assignment
        if (op == TOKEN_EQUAL) {
            ASTNode *node = ast_new(parser->arena, NODE_ASSIGN, location);
            node->assign.target = left;
            node->assign.value = parse_precedence(parser, precedence); // right-assoc
            left = node;
            continue;
        }

        // Compound assignment
        bool is_compound_assign = op == TOKEN_PLUS_EQUAL || op == TOKEN_MINUS_EQUAL ||
                                  op == TOKEN_STAR_EQUAL || op == TOKEN_SLASH_EQUAL;
        if (is_compound_assign) {
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

// ── If expression ──────────────────────────────────────────────────────

static ASTNode *parse_if(Parser *parser) {
    SourceLocation location = parser_current_location(parser);
    parser_expect(parser, TOKEN_IF);
    ASTNode *node = ast_new(parser->arena, NODE_IF, location);
    node->if_expression.condition = parser_parse_expression(parser);
    node->if_expression.then_body = parser_parse_block(parser);
    node->if_expression.else_body = NULL;
    parser_skip_newlines(parser);
    if (parser_match(parser, TOKEN_ELSE)) {
        parser_skip_newlines(parser);
        if (parser_check(parser, TOKEN_IF)) {
            node->if_expression.else_body = parse_if(parser);
        } else {
            node->if_expression.else_body = parser_parse_block(parser);
        }
    }
    return node;
}

// ── Public entry point ─────────────────────────────────────────────────

ASTNode *parser_parse_expression(Parser *parser) {
    if (parser_check(parser, TOKEN_IF)) {
        return parse_if(parser);
    }
    return parse_precedence(parser, PRECEDENCE_ASSIGN);
}
