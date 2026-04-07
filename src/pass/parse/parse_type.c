#include "_parse.h"

ASTType parser_parse_type(Parser *parser) {
    ASTType type = {.kind = AST_TYPE_NAME, .loc = parser_current_loc(parser)};

    // Option type: ?T
    if (parser_check(parser, TOKEN_QUESTION)) {
        type.kind = AST_TYPE_OPTION;
        parser_advance(parser); // consume '?'
        ASTType *elem = arena_alloc(parser->arena, sizeof(ASTType));
        *elem = parser_parse_type(parser);
        type.option_elem = elem;
        return type;
    }

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

    // Function type: fn(T1, T2) -> R
    if (parser_check(parser, TOKEN_FN) && parser->pos + 1 < parser->count &&
        parser->tokens[parser->pos + 1].kind == TOKEN_LEFT_PAREN) {
        type.kind = AST_TYPE_FN;
        type.fn_kind = FN_PLAIN;
        parser_advance(parser); // consume 'fn'
        parser_expect(parser, TOKEN_LEFT_PAREN);
        type.fn_param_types = NULL;
        if (!parser_check(parser, TOKEN_RIGHT_PAREN)) {
            do {
                ASTType *param = arena_alloc(parser->arena, sizeof(ASTType));
                *param = parser_parse_type(parser);
                BUF_PUSH(type.fn_param_types, param);
            } while (parser_match(parser, TOKEN_COMMA));
        }
        parser_expect(parser, TOKEN_RIGHT_PAREN);
        type.fn_return_type = NULL;
        if (parser_match(parser, TOKEN_ARROW)) {
            type.fn_return_type = arena_alloc(parser->arena, sizeof(ASTType));
            *type.fn_return_type = parser_parse_type(parser);
        }
        return type;
    }

    // Closure types: Fn(T1, T2) -> R  or  FnMut(T1, T2) -> R
    if (parser_check(parser, TOKEN_ID) && parser->pos + 1 < parser->count &&
        parser->tokens[parser->pos + 1].kind == TOKEN_LEFT_PAREN) {
        const char *name = parser_current_token(parser)->lexeme;
        FnTypeKind fk = FN_PLAIN;
        if (strcmp(name, "Fn") == 0) {
            fk = FN_CLOSURE;
        } else if (strcmp(name, "FnMut") == 0) {
            fk = FN_CLOSURE_MUT;
        }
        if (fk != FN_PLAIN) {
            type.kind = AST_TYPE_FN;
            type.fn_kind = fk;
            parser_advance(parser); // consume 'Fn' / 'FnMut'
            parser_expect(parser, TOKEN_LEFT_PAREN);
            type.fn_param_types = NULL;
            if (!parser_check(parser, TOKEN_RIGHT_PAREN)) {
                do {
                    ASTType *param = arena_alloc(parser->arena, sizeof(ASTType));
                    *param = parser_parse_type(parser);
                    BUF_PUSH(type.fn_param_types, param);
                } while (parser_match(parser, TOKEN_COMMA));
            }
            parser_expect(parser, TOKEN_RIGHT_PAREN);
            type.fn_return_type = NULL;
            if (parser_match(parser, TOKEN_ARROW)) {
                type.fn_return_type = arena_alloc(parser->arena, sizeof(ASTType));
                *type.fn_return_type = parser_parse_type(parser);
            }
            return type;
        }
    }

    if (token_is_type_keyword(parser_current_token(parser)->kind) ||
        parser_check(parser, TOKEN_ID)) {
        type.name = parser_advance(parser)->lexeme;
    } else {
        rsg_err(parser_current_loc(parser), "expected type name");
        type.kind = AST_TYPE_INFERRED;
        return type;
    }

    // Generic type args: Name<T, U, ...>
    type.type_args = NULL;
    if (parser_check(parser, TOKEN_LESS)) {
        parser_advance(parser); // consume '<'
        do {
            ASTType *arg = arena_alloc(parser->arena, sizeof(ASTType));
            *arg = parser_parse_type(parser);
            BUF_PUSH(type.type_args, arg);
        } while (parser_match(parser, TOKEN_COMMA));
        parser_expect(parser, TOKEN_GREATER);
    }

    // Result type: T ! E (postfix on any type)
    if (parser_check(parser, TOKEN_BANG)) {
        parser_advance(parser); // consume '!'
        ASTType result_type = {.kind = AST_TYPE_RESULT, .loc = type.loc};
        ASTType *ok_type = arena_alloc(parser->arena, sizeof(ASTType));
        *ok_type = type;
        ASTType *err_type = arena_alloc(parser->arena, sizeof(ASTType));
        *err_type = parser_parse_type(parser);
        result_type.result_ok = ok_type;
        result_type.result_err = err_type;
        return result_type;
    }

    return type;
}
