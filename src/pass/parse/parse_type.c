#include "_parse.h"

ASTType parser_parse_type(Parser *parser) {
    ASTType type = {.kind = AST_TYPE_NAME, .loc = parser_current_loc(parser)};

    // Pointer type: *T
    if (parser_check(parser, TOKEN_STAR)) {
        type.kind = AST_TYPE_PTR;
        parser_advance(parser); // consume '*'
        ASTType *pointee = arena_alloc(parser->arena, sizeof(ASTType));
        *pointee = parser_parse_type(parser);
        type.ptr_elem = pointee;
        return type;
    }

    // Array type: [N]T  or  Slice type: []T
    if (parser_check(parser, TOKEN_LEFT_BRACKET)) {
        parser_advance(parser); // consume '['

        // Slice type: []T (no size)
        if (parser_check(parser, TOKEN_RIGHT_BRACKET)) {
            type.kind = AST_TYPE_SLICE;
            parser_advance(parser); // consume ']'
            ASTType *elem = arena_alloc(parser->arena, sizeof(ASTType));
            *elem = parser_parse_type(parser);
            type.slice_elem = elem;
            return type;
        }

        // Array type: [N]T
        type.kind = AST_TYPE_ARRAY;
        if (!parser_check(parser, TOKEN_INTEGER_LIT)) {
            rsg_err(parser_current_loc(parser), "expected array size");
            type.kind = AST_TYPE_INFERRED;
            return type;
        }
        type.array_size = (int32_t)parser_current_token(parser)->lit_value.integer_value;
        parser_advance(parser); // consume size
        parser_expect(parser, TOKEN_RIGHT_BRACKET);
        ASTType *elem = arena_alloc(parser->arena, sizeof(ASTType));
        *elem = parser_parse_type(parser);
        type.array_elem = elem;
        return type;
    }

    // Tuple type: (T, U, ...)
    if (parser_check(parser, TOKEN_LEFT_PAREN)) {
        type.kind = AST_TYPE_TUPLE;
        type.tuple_elems = NULL;
        parser_advance(parser); // consume '('
        if (parser_match(parser, TOKEN_RIGHT_PAREN)) {
            parser->err_count++;
            rsg_err(type.loc, "empty tuple '()' is not a valid type; use 'unit'");
            type.kind = AST_TYPE_NAME;
            type.name = "unit";
            return type;
        }
        do {
            ASTType *elem = arena_alloc(parser->arena, sizeof(ASTType));
            *elem = parser_parse_type(parser);
            BUF_PUSH(type.tuple_elems, elem);
        } while (parser_match(parser, TOKEN_COMMA));
        parser_expect(parser, TOKEN_RIGHT_PAREN);
        return type;
    }

    if (token_is_type_keyword(parser_current_token(parser)->kind) ||
        parser_check(parser, TOKEN_ID)) {
        type.name = parser_advance(parser)->lexeme;
    } else {
        rsg_err(parser_current_loc(parser), "expected type name");
        type.kind = AST_TYPE_INFERRED;
    }
    return type;
}
