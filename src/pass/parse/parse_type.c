#include "_parse.h"

/** Parse fn/Fn/FnMut param types and optional return type after '(' has been consumed. */
static ASTType parse_fn_type_params(Parser *parser, FnTypeKind fn_kind, SrcLoc loc) {
    ASTType type = {.kind = AST_TYPE_FN, .fn_kind = fn_kind, .loc = loc};
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

// ── Prefix type sub-parsers ────────────────────────────────────────

/** Parse option sugar: ?T → Option<T>. */
static ASTType parse_option_type(Parser *parser, SrcLoc loc) {
    parser_advance(parser); // consume '?'
    ASTType *elem = arena_alloc(parser->arena, sizeof(ASTType));
    *elem = parser_parse_type(parser);
    ASTType type = {.kind = AST_TYPE_NAME, .name = "Option", .loc = loc, .type_args = NULL};
    BUF_PUSH(type.type_args, elem);
    return type;
}

/** Parse pointer type: *T */
static ASTType parse_ptr_type(Parser *parser, SrcLoc loc) {
    ASTType type = {.kind = AST_TYPE_PTR, .loc = loc};
    parser_advance(parser); // consume '*'
    ASTType *pointee = arena_alloc(parser->arena, sizeof(ASTType));
    *pointee = parser_parse_type(parser);
    type.ptr_elem = pointee;
    return type;
}

/** Parse array type [N]T or slice type []T. */
static ASTType parse_bracket_type(Parser *parser, SrcLoc loc) {
    ASTType type = {.kind = AST_TYPE_SLICE, .loc = loc};
    parser_advance(parser); // consume '['

    // Slice type: []T (no size)
    if (parser_check(parser, TOKEN_RIGHT_BRACKET)) {
        parser_advance(parser); // consume ']'
        ASTType *elem = arena_alloc(parser->arena, sizeof(ASTType));
        *elem = parser_parse_type(parser);
        type.slice_elem = elem;
        return type;
    }

    // Array type: [N]T
    type.kind = AST_TYPE_ARRAY;
    type.array_size_name = NULL;
    if (parser_check(parser, TOKEN_INTEGER_LIT)) {
        type.array_size = (int32_t)parser_current_token(parser)->lit_value.integer_value;
        parser_advance(parser); // consume size
    } else if (parser_check(parser, TOKEN_ID)) {
        type.array_size = 0;
        type.array_size_name = parser_current_token(parser)->lexeme;
        parser_advance(parser); // consume identifier
    } else {
        PARSER_ERR(parser, parser_current_loc(parser), "expected array size");
        type.kind = AST_TYPE_INFERRED;
        return type;
    }
    parser_expect(parser, TOKEN_RIGHT_BRACKET);
    ASTType *elem = arena_alloc(parser->arena, sizeof(ASTType));
    *elem = parser_parse_type(parser);
    type.array_elem = elem;
    return type;
}

/** Parse tuple type: (T, U, ...) */
static ASTType parse_tuple_type(Parser *parser, SrcLoc loc) {
    ASTType type = {.kind = AST_TYPE_TUPLE, .loc = loc, .tuple_elems = NULL};
    parser_advance(parser); // consume '('
    if (parser_match(parser, TOKEN_RIGHT_PAREN)) {
        type.kind = AST_TYPE_NAME;
        type.name = "unit";
        return type;
    }
    do {
        ASTType *elem = arena_alloc(parser->arena, sizeof(ASTType));
        *elem = parser_parse_type(parser);
        BUF_PUSH(type.tuple_elems, elem);
    } while (parser_match(parser, TOKEN_COMMA) && !parser_check(parser, TOKEN_RIGHT_PAREN));
    parser_expect(parser, TOKEN_RIGHT_PAREN);
    return type;
}

// ── Generic type args & postfix ────────────────────────────────────

/** Parse optional generic type args: <T, U, ...> after a named type. */
static void parse_generic_args(Parser *parser, ASTType *type) {
    type->type_args = NULL;
    if (!parser_check(parser, TOKEN_LESS)) {
        return;
    }
    parser_advance(parser); // consume '<'
    do {
        ASTType *arg = arena_alloc(parser->arena, sizeof(ASTType));
        if (parser_check(parser, TOKEN_INTEGER_LIT)) {
            // Comptime integer arg (e.g., 5 in ArrayWrapper<i32, 5>)
            arg->kind = AST_TYPE_COMPTIME_INT;
            arg->loc = parser_current_loc(parser);
            arg->comptime_int_value =
                (int64_t)parser_current_token(parser)->lit_value.integer_value;
            parser_advance(parser); // consume integer
        } else {
            *arg = parser_parse_type(parser);
        }
        BUF_PUSH(type->type_args, arg);
    } while (parser_match(parser, TOKEN_COMMA));
    parser_expect(parser, TOKEN_GREATER);
}

/** If next token is '!', wrap @p base in a result type: T!E → Result<T, E>. */
static ASTType parse_result_postfix(Parser *parser, ASTType base) {
    if (!parser_check(parser, TOKEN_BANG)) {
        return base;
    }
    parser_advance(parser); // consume '!'
    ASTType *ok = arena_alloc(parser->arena, sizeof(ASTType));
    *ok = base;
    ASTType *err = arena_alloc(parser->arena, sizeof(ASTType));
    *err = parser_parse_type(parser);
    ASTType result = {.kind = AST_TYPE_NAME, .name = "Result", .loc = base.loc, .type_args = NULL};
    BUF_PUSH(result.type_args, ok);
    BUF_PUSH(result.type_args, err);
    return result;
}

// ── Main type dispatcher ───────────────────────────────────────────

ASTType parser_parse_type(Parser *parser) {
    SrcLoc loc = parser_current_loc(parser);

    if (parser_check(parser, TOKEN_QUESTION)) {
        return parse_option_type(parser, loc);
    }
    if (parser_check(parser, TOKEN_STAR)) {
        return parse_ptr_type(parser, loc);
    }
    if (parser_check(parser, TOKEN_LEFT_BRACKET)) {
        return parse_bracket_type(parser, loc);
    }
    if (parser_check(parser, TOKEN_LEFT_PAREN)) {
        return parse_tuple_type(parser, loc);
    }

    // Function type: fn(T1, T2) -> R
    if (parser_check(parser, TOKEN_FN) && parser->pos + 1 < parser->count &&
        parser->tokens[parser->pos + 1].kind == TOKEN_LEFT_PAREN) {
        SrcLoc fn_loc = parser_current_loc(parser);
        parser_advance(parser); // consume 'fn'
        parser_expect(parser, TOKEN_LEFT_PAREN);
        return parse_fn_type_params(parser, FN_PLAIN, fn_loc);
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
            SrcLoc fn_loc = parser_current_loc(parser);
            parser_advance(parser); // consume 'Fn' / 'FnMut'
            parser_expect(parser, TOKEN_LEFT_PAREN);
            return parse_fn_type_params(parser, fk, fn_loc);
        }
    }

    // Named type (including Self)
    ASTType type = {.kind = AST_TYPE_NAME, .loc = loc};
    if (token_is_type_keyword(parser_current_token(parser)->kind) ||
        parser_check(parser, TOKEN_ID) || parser_check(parser, TOKEN_SELF)) {
        type.name = parser_advance(parser)->lexeme;
    } else {
        PARSER_ERR(parser, parser_current_loc(parser), "expected type name");
        type.kind = AST_TYPE_INFERRED;
        return type;
    }

    // Associated type access: T::Item
    if (parser_match(parser, TOKEN_COLON_COLON)) {
        type.kind = AST_TYPE_ASSOC;
        type.assoc_member = parser_expect(parser, TOKEN_ID)->lexeme;
        return parse_result_postfix(parser, type);
    }

    parse_generic_args(parser, &type);
    return parse_result_postfix(parser, type);
}
