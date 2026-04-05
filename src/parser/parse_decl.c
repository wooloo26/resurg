#include "_parser.h"

// ── Method declaration (inside struct) ─────────────────────────────────

static ASTNode *parse_method_declaration(Parser *parser, const char *struct_name) {
    SourceLocation location = parser_current_location(parser);
    parser_expect(parser, TOKEN_FUNCTION);

    ASTNode *node = ast_new(parser->arena, NODE_FUNCTION_DECLARATION, location);
    node->function_declaration.is_public = false;
    node->function_declaration.name = parser_expect(parser, TOKEN_IDENTIFIER)->lexeme;
    node->function_declaration.parameters = NULL;
    node->function_declaration.owner_struct = struct_name;
    node->function_declaration.receiver_name = NULL;
    node->function_declaration.is_mut_receiver = false;

    parser_expect(parser, TOKEN_LEFT_PAREN);
    if (!parser_check(parser, TOKEN_RIGHT_PAREN)) {
        // First parameter: check for receiver syntax *name or mut *name
        if (parser_check(parser, TOKEN_STAR) || parser_check(parser, TOKEN_MUT)) {
            bool is_mut = false;
            if (parser_match(parser, TOKEN_MUT)) {
                is_mut = true;
            }
            parser_expect(parser, TOKEN_STAR);
            node->function_declaration.receiver_name =
                parser_expect(parser, TOKEN_IDENTIFIER)->lexeme;
            node->function_declaration.is_mut_receiver = is_mut;

            // Parse remaining parameters after receiver
            while (parser_match(parser, TOKEN_COMMA)) {
                SourceLocation ploc = parser_current_location(parser);
                ASTNode *param = ast_new(parser->arena, NODE_PARAMETER, ploc);
                param->parameter.name = parser_expect(parser, TOKEN_IDENTIFIER)->lexeme;
                parser_expect(parser, TOKEN_COLON);
                param->parameter.type = parser_parse_type(parser);
                BUFFER_PUSH(node->function_declaration.parameters, param);
            }
        } else {
            // Regular parameters (no receiver)
            do {
                SourceLocation ploc = parser_current_location(parser);
                ASTNode *param = ast_new(parser->arena, NODE_PARAMETER, ploc);
                param->parameter.name = parser_expect(parser, TOKEN_IDENTIFIER)->lexeme;
                parser_expect(parser, TOKEN_COLON);
                param->parameter.type = parser_parse_type(parser);
                BUFFER_PUSH(node->function_declaration.parameters, param);
            } while (parser_match(parser, TOKEN_COMMA));
        }
    }
    parser_expect(parser, TOKEN_RIGHT_PAREN);

    // Return type
    node->function_declaration.return_type.kind = AST_TYPE_INFERRED;
    if (parser_match(parser, TOKEN_ARROW)) {
        node->function_declaration.return_type = parser_parse_type(parser);
    }

    parser_skip_newlines(parser);

    // Body: block or = expr
    if (parser_check(parser, TOKEN_LEFT_BRACE)) {
        node->function_declaration.body = parser_parse_block(parser);
    } else if (parser_match(parser, TOKEN_EQUAL)) {
        node->function_declaration.body = parser_parse_expression(parser);
    } else {
        rsg_error(parser_current_location(parser), "expected method body");
    }

    return node;
}

// ── Enum declaration ───────────────────────────────────────────────────

static ASTNode *parse_enum_declaration(Parser *parser) {
    SourceLocation location = parser_current_location(parser);
    parser_expect(parser, TOKEN_ENUM);

    ASTNode *node = ast_new(parser->arena, NODE_ENUM_DECLARATION, location);
    node->enum_declaration.name = parser_expect(parser, TOKEN_IDENTIFIER)->lexeme;
    node->enum_declaration.variants = NULL;
    node->enum_declaration.methods = NULL;

    parser_skip_newlines(parser);
    parser_expect(parser, TOKEN_LEFT_BRACE);
    parser_skip_newlines(parser);

    while (!parser_check(parser, TOKEN_RIGHT_BRACE) && !parser_at_end(parser)) {
        if (parser_check(parser, TOKEN_FUNCTION)) {
            // Method declaration inside enum
            ASTNode *method = parse_method_declaration(parser, node->enum_declaration.name);
            BUFFER_PUSH(node->enum_declaration.methods, method);
        } else if (parser_check(parser, TOKEN_IDENTIFIER)) {
            ASTEnumVariant variant = {0};
            variant.name = parser_advance(parser)->lexeme;
            variant.kind = VARIANT_UNIT;
            variant.tuple_types = NULL;
            variant.fields = NULL;
            variant.discriminant = NULL;

            if (parser_match(parser, TOKEN_LEFT_PAREN)) {
                // Tuple variant: Name(type1, type2, ...)
                variant.kind = VARIANT_TUPLE;
                if (!parser_check(parser, TOKEN_RIGHT_PAREN)) {
                    do {
                        ASTType *elem = arena_alloc_zero(parser->arena, sizeof(ASTType));
                        *elem = parser_parse_type(parser);
                        BUFFER_PUSH(variant.tuple_types, *elem);
                    } while (parser_match(parser, TOKEN_COMMA));
                }
                parser_expect(parser, TOKEN_RIGHT_PAREN);
            } else if (parser_check(parser, TOKEN_LEFT_BRACE)) {
                // Struct variant: Name { field: type, ... }
                variant.kind = VARIANT_STRUCT;
                parser_expect(parser, TOKEN_LEFT_BRACE);
                parser_skip_newlines(parser);
                if (!parser_check(parser, TOKEN_RIGHT_BRACE)) {
                    do {
                        parser_skip_newlines(parser);
                        ASTStructField field = {0};
                        field.name = parser_expect(parser, TOKEN_IDENTIFIER)->lexeme;
                        parser_expect(parser, TOKEN_COLON);
                        field.type = parser_parse_type(parser);
                        field.default_value = NULL;
                        BUFFER_PUSH(variant.fields, field);
                    } while (parser_match(parser, TOKEN_COMMA));
                }
                parser_skip_newlines(parser);
                parser_expect(parser, TOKEN_RIGHT_BRACE);
            } else if (parser_match(parser, TOKEN_EQUAL)) {
                // Explicit discriminant: Name = expr
                variant.discriminant = parser_parse_expression(parser);
            }

            BUFFER_PUSH(node->enum_declaration.variants, variant);
        } else {
            rsg_error(parser_current_location(parser), "expected variant or method in enum");
            parser_advance(parser);
        }
        // Consume optional comma and newlines between variants
        parser_match(parser, TOKEN_COMMA);
        parser_skip_newlines(parser);
    }

    parser_expect(parser, TOKEN_RIGHT_BRACE);
    return node;
}

// ── Struct declaration ─────────────────────────────────────────────────

static ASTNode *parse_struct_declaration(Parser *parser) {
    SourceLocation location = parser_current_location(parser);
    parser_expect(parser, TOKEN_STRUCT);

    ASTNode *node = ast_new(parser->arena, NODE_STRUCT_DECLARATION, location);
    node->struct_declaration.name = parser_expect(parser, TOKEN_IDENTIFIER)->lexeme;
    node->struct_declaration.fields = NULL;
    node->struct_declaration.methods = NULL;
    node->struct_declaration.embedded = NULL;

    parser_skip_newlines(parser);
    parser_expect(parser, TOKEN_LEFT_BRACE);
    parser_skip_newlines(parser);

    while (!parser_check(parser, TOKEN_RIGHT_BRACE) && !parser_at_end(parser)) {
        if (parser_check(parser, TOKEN_FUNCTION)) {
            // Method declaration
            ASTNode *method = parse_method_declaration(parser, node->struct_declaration.name);
            BUFFER_PUSH(node->struct_declaration.methods, method);
        } else if (parser_check(parser, TOKEN_IDENTIFIER)) {
            const char *name = parser_advance(parser)->lexeme;

            if (parser_match(parser, TOKEN_COLON)) {
                // Field: name: Type [= default]
                ASTStructField field = {0};
                field.name = name;
                field.type = parser_parse_type(parser);
                field.default_value = NULL;
                if (parser_match(parser, TOKEN_EQUAL)) {
                    field.default_value = parser_parse_expression(parser);
                }
                BUFFER_PUSH(node->struct_declaration.fields, field);
            } else {
                // Embedded struct: just a type name on its own line
                BUFFER_PUSH(node->struct_declaration.embedded, name);
            }
        } else {
            rsg_error(parser_current_location(parser),
                      "expected field, method, or embedded struct");
            parser_advance(parser);
        }
        parser_skip_newlines(parser);
    }

    parser_expect(parser, TOKEN_RIGHT_BRACE);
    return node;
}

// ── Function declaration ───────────────────────────────────────────────

static ASTNode *parse_function_declaration(Parser *parser, bool is_public) {
    SourceLocation location = parser_current_location(parser);
    parser_expect(parser, TOKEN_FUNCTION);

    ASTNode *node = ast_new(parser->arena, NODE_FUNCTION_DECLARATION, location);
    node->function_declaration.is_public = is_public;
    node->function_declaration.name = parser_expect(parser, TOKEN_IDENTIFIER)->lexeme;
    node->function_declaration.parameters = NULL;
    node->function_declaration.receiver_name = NULL;
    node->function_declaration.is_mut_receiver = false;
    node->function_declaration.owner_struct = NULL;

    // Parameters
    parser_expect(parser, TOKEN_LEFT_PAREN);
    if (!parser_check(parser, TOKEN_RIGHT_PAREN)) {
        do {
            SourceLocation parameter_location = parser_current_location(parser);
            ASTNode *parameter = ast_new(parser->arena, NODE_PARAMETER, parameter_location);
            parameter->parameter.is_mut = parser_match(parser, TOKEN_MUT);
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

    // struct
    if (parser_check(parser, TOKEN_STRUCT)) {
        return parse_struct_declaration(parser);
    }

    // enum
    if (parser_check(parser, TOKEN_ENUM)) {
        return parse_enum_declaration(parser);
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
