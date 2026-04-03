#include "parser/_parser.h"

ASTType parser_parse_type(Parser *parser) {
    ASTType type = {.kind = AST_TYPE_NAME, .location = parser_current_location(parser)};

    // Array type: [N]T
    if (parser_check(parser, TOKEN_LEFT_BRACKET)) {
        type.kind = AST_TYPE_ARRAY;
        parser_advance(parser); // consume '['
        if (!parser_check(parser, TOKEN_INTEGER_LITERAL)) {
            rsg_error(parser_current_location(parser), "expected array size");
            type.kind = AST_TYPE_INFERRED;
            return type;
        }
        type.array_size = (int32_t)parser_current_token(parser)->literal_value.integer_value;
        parser_advance(parser); // consume size
        parser_expect(parser, TOKEN_RIGHT_BRACKET);
        ASTType *element = arena_alloc(parser->arena, sizeof(ASTType));
        *element = parser_parse_type(parser);
        type.array_element = element;
        return type;
    }

    // Tuple type: (T, U, ...)
    if (parser_check(parser, TOKEN_LEFT_PAREN)) {
        type.kind = AST_TYPE_TUPLE;
        type.tuple_elements = NULL;
        parser_advance(parser); // consume '('
        if (!parser_check(parser, TOKEN_RIGHT_PAREN)) {
            do {
                ASTType *element = arena_alloc(parser->arena, sizeof(ASTType));
                *element = parser_parse_type(parser);
                BUFFER_PUSH(type.tuple_elements, element);
            } while (parser_match(parser, TOKEN_COMMA));
        }
        parser_expect(parser, TOKEN_RIGHT_PAREN);
        return type;
    }

    if (token_is_type_keyword(parser_current_token(parser)->kind) || parser_check(parser, TOKEN_IDENTIFIER)) {
        type.name = parser_advance(parser)->lexeme;
    } else {
        rsg_error(parser_current_location(parser), "expected type name");
        type.kind = AST_TYPE_INFERRED;
    }
    return type;
}
