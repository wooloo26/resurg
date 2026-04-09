#include "_parse.h"

// ── Forward declarations ────────────────────────────────────────────

static ASTNode *parse_precedence(Parser *parser, Precedence minimum_precedence);

// ── Leaf / primary parsers ────────────────────────────────────────────

static ASTNode *parse_str_interpolation(Parser *parser, SrcLoc loc) {
    ASTNode *interpolation = ast_new(parser->arena, NODE_STR_INTERPOLATION, loc);
    interpolation->str_interpolation.parts = NULL;

    ASTNode *text = ast_new(parser->arena, NODE_LIT, loc);
    text->lit.kind = LIT_STR;
    text->lit.str_value = parser_previous_token(parser)->lit_value.str_value;
    text->lit.str_len = parser_previous_token(parser)->len;
    BUF_PUSH(interpolation->str_interpolation.parts, text);

    while (parser_match(parser, TOKEN_INTERPOLATION_START)) {
        ASTNode *expr = parser_parse_expr(parser);
        BUF_PUSH(interpolation->str_interpolation.parts, expr);
        parser_expect(parser, TOKEN_INTERPOLATION_END);

        SrcLoc text_loc = parser_current_loc(parser);
        parser_expect(parser, TOKEN_STR_LIT);
        ASTNode *text2 = ast_new(parser->arena, NODE_LIT, text_loc);
        text2->lit.kind = LIT_STR;
        text2->lit.str_value = parser_previous_token(parser)->lit_value.str_value;
        text2->lit.str_len = parser_previous_token(parser)->len;
        BUF_PUSH(interpolation->str_interpolation.parts, text2);
    }
    return interpolation;
}

/**
 * Parse a comma-separated list of exprs, appending each to @p buf.
 * Assumes the list has at least one elem.  Skips newlines between items.
 */
static void parse_comma_separated(Parser *parser, ASTNode ***buf) {
    do {
        parser_skip_newlines(parser);
        BUF_PUSH(*buf, parser_parse_expr(parser));
    } while (parser_match(parser, TOKEN_COMMA));
}

/**
 * Parse a struct lit body: { field = expr, ... }.
 * The struct name id has already been consumed and passed as @p name_node.
 */
static ASTNode *parse_struct_lit(Parser *parser, ASTNode *name_node) {
    SrcLoc loc = name_node->loc;
    parser_expect(parser, TOKEN_LEFT_BRACE);
    parser_skip_newlines(parser);

    ASTNode *node = ast_new(parser->arena, NODE_STRUCT_LIT, loc);
    node->struct_lit.name = name_node->id.name;
    node->struct_lit.field_names = NULL;
    node->struct_lit.field_values = NULL;

    if (!parser_check(parser, TOKEN_RIGHT_BRACE)) {
        do {
            parser_skip_newlines(parser);
            const char *field_name = parser_expect(parser, TOKEN_ID)->lexeme;
            parser_expect(parser, TOKEN_EQUAL);
            ASTNode *value = parser_parse_expr(parser);
            BUF_PUSH(node->struct_lit.field_names, field_name);
            BUF_PUSH(node->struct_lit.field_values, value);
        } while (parser_match(parser, TOKEN_COMMA));
    }
    parser_skip_newlines(parser);
    parser_expect(parser, TOKEN_RIGHT_BRACE);
    return node;
}

/** Return true if current pos looks like a struct lit: IDENT { }  or IDENT { IDENT = ... }
 */
static bool is_struct_lit_ahead(const Parser *parser) {
    if (!parser_check(parser, TOKEN_LEFT_BRACE)) {
        return false;
    }
    int32_t pos = parser->pos + 1;
    // Skip newlines
    while (pos < parser->count && parser->tokens[pos].kind == TOKEN_NEWLINE) {
        pos++;
    }
    if (pos >= parser->count) {
        return false;
    }
    // Empty struct lit: { }
    if (parser->tokens[pos].kind == TOKEN_RIGHT_BRACE) {
        return true;
    }
    // Named field: IDENT =
    if (parser->tokens[pos].kind == TOKEN_ID && pos + 1 < parser->count) {
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

/** Parse Enum::Variant { field = expr, ... } from a member node. */
static ASTNode *parse_enum_struct_lit(Parser *parser, ASTNode *member_node) {
    ASTNode *node = ast_new(parser->arena, NODE_ENUM_INIT, member_node->loc);
    node->enum_init.enum_name = member_node->member.object->id.name;
    node->enum_init.variant_name = member_node->member.member;
    node->enum_init.args = NULL;
    node->enum_init.field_names = NULL;
    node->enum_init.field_values = NULL;

    parser_expect(parser, TOKEN_LEFT_BRACE);
    parser_skip_newlines(parser);
    if (!parser_check(parser, TOKEN_RIGHT_BRACE)) {
        do {
            parser_skip_newlines(parser);
            const char *field_name = parser_expect(parser, TOKEN_ID)->lexeme;
            parser_expect(parser, TOKEN_EQUAL);
            ASTNode *value = parser_parse_expr(parser);
            BUF_PUSH(node->enum_init.field_names, field_name);
            BUF_PUSH(node->enum_init.field_values, value);
        } while (parser_match(parser, TOKEN_COMMA));
    }
    parser_skip_newlines(parser);
    parser_expect(parser, TOKEN_RIGHT_BRACE);
    return node;
}

/**
 * Parse an array lit: [expr, ...] or [N]T{expr, ...}.
 * The opening '[' has NOT been consumed yet.
 */
static ASTNode *parse_array_lit(Parser *parser) {
    SrcLoc loc = parser_current_loc(parser);
    parser_expect(parser, TOKEN_LEFT_BRACKET);

    // Check for slice lit: []T{values}
    // Pattern: ']', type-keyword-or-id, '{'
    if (parser_check(parser, TOKEN_RIGHT_BRACKET)) {
        int32_t save_pos = parser->pos;
        parser_advance(parser); // consume ']'
        if (token_is_type_keyword(parser_current_token(parser)->kind) ||
            parser_check(parser, TOKEN_ID) || parser_check(parser, TOKEN_LEFT_BRACKET) ||
            parser_check(parser, TOKEN_FN)) {
            // This is []T{values} — a slice lit
            ASTNode *node = ast_new(parser->arena, NODE_SLICE_LIT, loc);
            node->slice_lit.elem_type = parser_parse_type(parser);
            node->slice_lit.elems = NULL;
            parser_expect(parser, TOKEN_LEFT_BRACE);
            if (!parser_check(parser, TOKEN_RIGHT_BRACE)) {
                parse_comma_separated(parser, &node->slice_lit.elems);
            }
            parser_expect(parser, TOKEN_RIGHT_BRACE);
            return node;
        }
        // Not a slice lit, restore pos — this is an empty array []
        parser->pos = save_pos;
    }

    // Check if this is [N]T{values} (typed array lit)
    // Pattern: INTEGER_LIT, ']', type-keyword-or-id
    bool has_size_and_bracket = parser_check(parser, TOKEN_INTEGER_LIT) &&
                                parser->pos + 1 < parser->count &&
                                parser->tokens[parser->pos + 1].kind == TOKEN_RIGHT_BRACKET;
    bool has_type_after = has_size_and_bracket && parser->pos + 2 < parser->count &&
                          (token_is_type_keyword(parser->tokens[parser->pos + 2].kind) ||
                           parser->tokens[parser->pos + 2].kind == TOKEN_ID ||
                           parser->tokens[parser->pos + 2].kind == TOKEN_FN);
    if (has_type_after) {
        int32_t size = (int32_t)parser_current_token(parser)->lit_value.integer_value;
        parser_advance(parser); // consume size
        parser_advance(parser); // consume ']'

        // Now parse elem type
        ASTNode *node = ast_new(parser->arena, NODE_ARRAY_LIT, loc);
        node->array_lit.elem_type = parser_parse_type(parser);
        node->array_lit.size = size;
        node->array_lit.elems = NULL;

        // Parse {values}
        parser_expect(parser, TOKEN_LEFT_BRACE);
        if (!parser_check(parser, TOKEN_RIGHT_BRACE)) {
            parse_comma_separated(parser, &node->array_lit.elems);
        }
        parser_expect(parser, TOKEN_RIGHT_BRACE);
        return node;
    }

    // Simple array lit: [expr, expr, ...]
    ASTNode *node = ast_new(parser->arena, NODE_ARRAY_LIT, loc);
    node->array_lit.elem_type.kind = AST_TYPE_INFERRED;
    node->array_lit.elems = NULL;
    if (!parser_check(parser, TOKEN_RIGHT_BRACKET)) {
        parse_comma_separated(parser, &node->array_lit.elems);
    }
    node->array_lit.size = BUF_LEN(node->array_lit.elems);
    parser_expect(parser, TOKEN_RIGHT_BRACKET);
    return node;
}

/** Parse a closure expression: |params| body  or  || body. */
static ASTNode *parse_closure(Parser *parser, SrcLoc loc) {
    ASTNode *node = ast_new(parser->arena, NODE_CLOSURE, loc);
    node->closure.params = NULL;
    node->closure.return_type.kind = AST_TYPE_INFERRED;

    if (parser_match(parser, TOKEN_PIPE_PIPE)) {
        // Zero-param closure: || body
    } else {
        parser_advance(parser); // consume '|'
        if (!parser_check(parser, TOKEN_PIPE)) {
            do {
                SrcLoc param_loc = parser_current_loc(parser);
                ASTNode *param = ast_new(parser->arena, NODE_PARAM, param_loc);
                param->param.is_mut = false;
                param->param.name = parser_expect(parser, TOKEN_ID)->lexeme;
                if (parser_match(parser, TOKEN_COLON)) {
                    param->param.type = parser_parse_type(parser);
                } else {
                    param->param.type.kind = AST_TYPE_INFERRED;
                }
                BUF_PUSH(node->closure.params, param);
            } while (parser_match(parser, TOKEN_COMMA));
        }
        parser_expect(parser, TOKEN_PIPE);
    }

    if (parser_match(parser, TOKEN_ARROW)) {
        node->closure.return_type = parser_parse_type(parser);
    }
    if (parser_check(parser, TOKEN_LEFT_BRACE)) {
        node->closure.body = parser_parse_block(parser);
    } else {
        node->closure.body = parse_precedence(parser, PRECEDENCE_PIPE + 1);
    }
    return node;
}

static ASTNode *parse_primary(Parser *parser) {
    SrcLoc loc = parser_current_loc(parser);

    if (parser_match(parser, TOKEN_INTEGER_LIT)) {
        ASTNode *node = ast_new(parser->arena, NODE_LIT, loc);
        node->lit.kind = LIT_I32;
        node->lit.integer_value = parser_previous_token(parser)->lit_value.integer_value;
        return node;
    }
    if (parser_match(parser, TOKEN_FLOAT_LIT)) {
        ASTNode *node = ast_new(parser->arena, NODE_LIT, loc);
        node->lit.kind = LIT_F64;
        node->lit.float64_value = parser_previous_token(parser)->lit_value.float_value;
        return node;
    }
    if (parser_match(parser, TOKEN_CHAR_LIT)) {
        ASTNode *node = ast_new(parser->arena, NODE_LIT, loc);
        node->lit.kind = LIT_CHAR;
        node->lit.char_value = parser_previous_token(parser)->lit_value.char_value;
        return node;
    }
    if (parser_match(parser, TOKEN_STR_LIT)) {
        if (parser_check(parser, TOKEN_INTERPOLATION_START)) {
            return parse_str_interpolation(parser, loc);
        }
        ASTNode *node = ast_new(parser->arena, NODE_LIT, loc);
        node->lit.kind = LIT_STR;
        node->lit.str_value = parser_previous_token(parser)->lit_value.str_value;
        node->lit.str_len = parser_previous_token(parser)->len;
        return node;
    }
    if (parser_match(parser, TOKEN_TRUE)) {
        ASTNode *node = ast_new(parser->arena, NODE_LIT, loc);
        node->lit.kind = LIT_BOOL;
        node->lit.boolean_value = true;
        return node;
    }
    if (parser_match(parser, TOKEN_FALSE)) {
        ASTNode *node = ast_new(parser->arena, NODE_LIT, loc);
        node->lit.kind = LIT_BOOL;
        node->lit.boolean_value = false;
        return node;
    }
    if (parser_match(parser, TOKEN_UNIT)) {
        ASTNode *node = ast_new(parser->arena, NODE_LIT, loc);
        node->lit.kind = LIT_UNIT;
        return node;
    }

    // Typed lit syntax: type_keyword(expr) e.g. i64(100), f32(3.14)
    if (token_is_type_keyword(parser_current_token(parser)->kind) &&
        parser->pos + 1 < parser->count &&
        parser->tokens[parser->pos + 1].kind == TOKEN_LEFT_PAREN) {
        ASTNode *node = ast_new(parser->arena, NODE_TYPE_CONVERSION, loc);
        node->type_conversion.target_type.kind = AST_TYPE_NAME;
        node->type_conversion.target_type.name = parser_advance(parser)->lexeme;
        node->type_conversion.target_type.loc = loc;
        parser_expect(parser, TOKEN_LEFT_PAREN);
        node->type_conversion.operand = parser_parse_expr(parser);
        parser_expect(parser, TOKEN_RIGHT_PAREN);
        return node;
    }

    if (parser_match(parser, TOKEN_ID) || parser_match(parser, TOKEN_SELF)) {
        ASTNode *node = ast_new(parser->arena, NODE_ID, loc);
        node->id.name = parser_previous_token(parser)->lexeme;
        return node;
    }

    // Array lit: [N]T{elems} (typed) or handled via var decl ctx
    if (parser_check(parser, TOKEN_LEFT_BRACKET)) {
        return parse_array_lit(parser);
    }

    // Parenthesized expr or tuple lit
    if (parser_match(parser, TOKEN_LEFT_PAREN)) {
        if (parser_match(parser, TOKEN_RIGHT_PAREN)) {
            ASTNode *node = ast_new(parser->arena, NODE_LIT, loc);
            node->lit.kind = LIT_UNIT;
            return node;
        }
        ASTNode *first = parser_parse_expr(parser);
        if (parser_match(parser, TOKEN_COMMA)) {
            // Tuple lit: (expr,) or (expr, expr, ...)
            ASTNode *node = ast_new(parser->arena, NODE_TUPLE_LIT, loc);
            node->tuple_lit.elems = NULL;
            BUF_PUSH(node->tuple_lit.elems, first);
            if (!parser_check(parser, TOKEN_RIGHT_PAREN)) {
                parse_comma_separated(parser, &node->tuple_lit.elems);
            }
            parser_expect(parser, TOKEN_RIGHT_PAREN);
            return node;
        }
        parser_expect(parser, TOKEN_RIGHT_PAREN);
        return first;
    }

    if (parser_check(parser, TOKEN_IF)) {
        return parser_parse_expr(parser);
    }
    if (parser_check(parser, TOKEN_LOOP)) {
        return parser_parse_stmt(parser);
    }
    if (parser_check(parser, TOKEN_LEFT_BRACE)) {
        return parser_parse_block(parser);
    }

    // Closure: |params| body  or  || body
    if (parser_check(parser, TOKEN_PIPE) || parser_check(parser, TOKEN_PIPE_PIPE)) {
        return parse_closure(parser, loc);
    }
    const char *token_name = token_kind_str(parser_current_token(parser)->kind);
    rsg_err(loc, "expected expr, got '%s'", token_name);
    parser_advance(parser);
    return ast_new(parser->arena, NODE_LIT, loc); // err recovery
}

// ── Postfix / unary / precedence climbing ──────────────────────────────

/** Parse a comma-separated arg list (posal, named, and mut). */
static ASTNode *parse_call_args(Parser *parser, ASTNode *callee, SrcLoc loc) {
    ASTNode *node = ast_new(parser->arena, NODE_CALL, loc);
    node->call.callee = callee;
    node->call.args = NULL;
    node->call.arg_names = NULL;
    node->call.arg_is_mut = NULL;
    if (!parser_check(parser, TOKEN_RIGHT_PAREN)) {
        do {
            parser_skip_newlines(parser);
            bool is_mut_arg = parser_match(parser, TOKEN_MUT);
            bool is_named_arg = parser_check(parser, TOKEN_ID) && parser->pos + 1 < parser->count &&
                                parser->tokens[parser->pos + 1].kind == TOKEN_EQUAL;
            if (is_named_arg) {
                const char *arg_name = parser_advance(parser)->lexeme;
                parser_advance(parser); // consume '='
                ASTNode *value = parser_parse_expr(parser);
                BUF_PUSH(node->call.args, value);
                BUF_PUSH(node->call.arg_names, arg_name);
                BUF_PUSH(node->call.arg_is_mut, is_mut_arg);
            } else {
                ASTNode *arg = parser_parse_expr(parser);
                BUF_PUSH(node->call.args, arg);
                BUF_PUSH(node->call.arg_names, (const char *)NULL);
                BUF_PUSH(node->call.arg_is_mut, is_mut_arg);
            }
        } while (parser_match(parser, TOKEN_COMMA));
    }
    parser_expect(parser, TOKEN_RIGHT_PAREN);
    return node;
}

/**
 * Try to parse a generic instantiation: Name<Type, ...> followed by
 * '(' (fn call), '{' (struct lit), or '::' (enum access).
 * Returns the resulting node on success, or NULL if this isn't a generic
 * (parser position is restored on failure).
 */
static ASTNode *parse_generic_postfix(Parser *parser, ASTNode *left, SrcLoc loc) {
    int32_t save_pos = parser->pos;
    parser_advance(parser); // consume '<'

    ASTType *type_args = NULL;
    bool valid = true;

    do {
        if (!(token_is_type_keyword(parser_current_token(parser)->kind) ||
              parser_check(parser, TOKEN_ID) || parser_check(parser, TOKEN_LEFT_BRACKET) ||
              parser_check(parser, TOKEN_LEFT_PAREN) || parser_check(parser, TOKEN_STAR) ||
              parser_check(parser, TOKEN_INTEGER_LIT))) {
            valid = false;
            break;
        }
        ASTType arg;
        if (parser_check(parser, TOKEN_INTEGER_LIT)) {
            // Comptime integer arg
            arg = (ASTType){.kind = AST_TYPE_COMPTIME_INT,
                            .loc = parser_current_loc(parser),
                            .comptime_int_value =
                                (int64_t)parser_current_token(parser)->lit_value.integer_value};
            parser_advance(parser); // consume integer
        } else {
            arg = parser_parse_type(parser);
        }
        BUF_PUSH(type_args, arg);
        if (!parser_check(parser, TOKEN_COMMA) && !parser_check(parser, TOKEN_GREATER)) {
            valid = false;
            break;
        }
    } while (parser_match(parser, TOKEN_COMMA));

    if (!valid || !parser_match(parser, TOKEN_GREATER)) {
        BUF_FREE(type_args);
        parser->pos = save_pos;
        return NULL;
    }

    // Generic fn call: Name<Type, ...>(args)
    if (parser_match(parser, TOKEN_LEFT_PAREN)) {
        ASTNode *call = parse_call_args(parser, left, loc);
        call->call.type_args = type_args;
        return call;
    }

    // Generic struct lit: Name<Type, ...> { field = value, ... }
    if (is_struct_lit_ahead(parser)) {
        ASTNode *slit = parse_struct_lit(parser, left);
        slit->struct_lit.type_args = type_args;
        return slit;
    }

    // Generic enum access: Name<Type, ...>::Variant
    if (parser_check(parser, TOKEN_COLON_COLON)) {
        parser_advance(parser); // consume '::'
        const char *variant_name = parser_expect(parser, TOKEN_ID)->lexeme;
        ASTNode *member = ast_new(parser->arena, NODE_MEMBER, loc);
        member->member.object = left;
        member->member.member = variant_name;

        if (parser_match(parser, TOKEN_LEFT_PAREN)) {
            ASTNode *call = parse_call_args(parser, member, loc);
            call->call.type_args = type_args;
            return call;
        }
        if (is_struct_lit_ahead(parser)) {
            ASTNode *node = parse_enum_struct_lit(parser, member);
            node->enum_init.type_args = type_args;
            return node;
        }
        // Unit variant: Enum<T>::Variant (no args)
        ASTNode *call = ast_new(parser->arena, NODE_CALL, loc);
        call->call.callee = member;
        call->call.args = NULL;
        call->call.arg_names = NULL;
        call->call.arg_is_mut = NULL;
        call->call.type_args = type_args;
        return call;
    }

    // Not followed by (, {, or :: — backtrack
    BUF_FREE(type_args);
    parser->pos = save_pos;
    return NULL;
}

/** Parse an index or slice expression after '[' has been consumed. */
static ASTNode *parse_index_or_slice(Parser *parser, ASTNode *object, SrcLoc loc) {
    // obj[..] or obj[..end]
    if (parser_check(parser, TOKEN_DOT_DOT)) {
        parser_advance(parser); // consume '..'
        ASTNode *node = ast_new(parser->arena, NODE_SLICE_EXPR, loc);
        node->slice_expr.object = object;
        node->slice_expr.start = NULL;
        if (parser_check(parser, TOKEN_RIGHT_BRACKET)) {
            node->slice_expr.end = NULL;
            node->slice_expr.full_range = true;
        } else {
            node->slice_expr.end = parser_parse_expr(parser);
            node->slice_expr.full_range = false;
        }
        parser_expect(parser, TOKEN_RIGHT_BRACKET);
        return node;
    }

    ASTNode *idx_or_start = parser_parse_expr(parser);

    // obj[start..end] or obj[start..]
    if (parser_check(parser, TOKEN_DOT_DOT)) {
        parser_advance(parser); // consume '..'
        ASTNode *node = ast_new(parser->arena, NODE_SLICE_EXPR, loc);
        node->slice_expr.object = object;
        node->slice_expr.start = idx_or_start;
        node->slice_expr.full_range = false;
        if (parser_check(parser, TOKEN_RIGHT_BRACKET)) {
            node->slice_expr.end = NULL;
        } else {
            node->slice_expr.end = parser_parse_expr(parser);
        }
        parser_expect(parser, TOKEN_RIGHT_BRACKET);
        return node;
    }

    // Regular idx access: obj[idx]
    ASTNode *node = ast_new(parser->arena, NODE_IDX, loc);
    node->idx_access.object = object;
    node->idx_access.idx = idx_or_start;
    parser_expect(parser, TOKEN_RIGHT_BRACKET);
    return node;
}

static ASTNode *parse_postfix(Parser *parser) {
    ASTNode *left = parse_primary(parser);
    for (;;) {
        SrcLoc loc = parser_current_loc(parser);

        // Struct lit: Identifier { field = expr, ... }
        if (!parser->no_struct_lit && left->kind == NODE_ID && is_struct_lit_ahead(parser)) {
            left = parse_struct_lit(parser, left);
            continue;
        }

        // Enum struct variant lit: Enum::Variant { field = expr, ... }
        if (!parser->no_struct_lit && left->kind == NODE_MEMBER &&
            left->member.object->kind == NODE_ID && is_struct_lit_ahead(parser)) {
            left = parse_enum_struct_lit(parser, left);
            continue;
        }

        // Generic call / struct lit / enum access: Name<Type, ...>(...)
        if (left->kind == NODE_ID && parser_check(parser, TOKEN_LESS)) {
            ASTNode *result = parse_generic_postfix(parser, left, loc);
            if (result != NULL) {
                left = result;
                continue;
            }
        }

        if (parser_match(parser, TOKEN_LEFT_PAREN)) {
            left = parse_call_args(parser, left, loc);
            continue;
        }
        if (parser_match(parser, TOKEN_LEFT_BRACKET)) {
            left = parse_index_or_slice(parser, left, loc);
            continue;
        }
        if (parser_match(parser, TOKEN_DOT)) {
            // Tuple member access: t.0, t.1, or struct/module access: t.name
            if (parser_check(parser, TOKEN_INTEGER_LIT)) {
                ASTNode *node = ast_new(parser->arena, NODE_MEMBER, loc);
                node->member.object = left;
                unsigned long long idx_value =
                    (unsigned long long)parser_current_token(parser)->lit_value.integer_value;
                node->member.member = arena_sprintf(parser->arena, "%llu", idx_value);
                parser_advance(parser);
                left = node;
            } else {
                ASTNode *node = ast_new(parser->arena, NODE_MEMBER, loc);
                node->member.object = left;
                node->member.member = parser_expect(parser, TOKEN_ID)->lexeme;
                left = node;
            }
            continue;
        }
        // Namespace access: Enum::Variant, Enum::method(args)
        if (parser_match(parser, TOKEN_COLON_COLON)) {
            ASTNode *node = ast_new(parser->arena, NODE_MEMBER, loc);
            node->member.object = left;
            node->member.member = parser_expect(parser, TOKEN_ID)->lexeme;
            left = node;
            continue;
        }
        // Optional chaining: expr?.member
        if (parser_match(parser, TOKEN_QUESTION_DOT)) {
            ASTNode *node = ast_new(parser->arena, NODE_OPTIONAL_CHAIN, loc);
            node->optional_chain.object = left;
            node->optional_chain.member = parser_expect(parser, TOKEN_ID)->lexeme;
            left = node;
            continue;
        }
        // Postfix try / error propagation: expr!
        if (parser_check(parser, TOKEN_BANG)) {
            // Distinguish postfix ! from prefix unary ! by checking if next token
            // could start an expression (in which case it's binary/prefix, not postfix).
            // Postfix ! should be followed by non-expression starters.
            int32_t next_pos = parser->pos + 1;
            if (next_pos < parser->count) {
                TokenKind next = parser->tokens[next_pos].kind;
                // If the ! is followed by an expression-starter, it's prefix unary, not postfix.
                // Otherwise (newline, semicolon, }, ), comma, etc.) it's postfix.
                bool is_postfix =
                    (next == TOKEN_NEWLINE || next == TOKEN_SEMICOLON ||
                     next == TOKEN_RIGHT_PAREN || next == TOKEN_RIGHT_BRACE ||
                     next == TOKEN_RIGHT_BRACKET || next == TOKEN_COMMA || next == TOKEN_EOF ||
                     next == TOKEN_DOT || next == TOKEN_QUESTION_DOT);
                if (is_postfix) {
                    parser_advance(parser); // consume '!'
                    ASTNode *node = ast_new(parser->arena, NODE_TRY, loc);
                    node->try_expr.operand = left;
                    left = node;
                    continue;
                }
            }
        }
        break;
    }
    return left;
}

static ASTNode *parse_unary(Parser *parser) {
    if (parser_check(parser, TOKEN_MINUS) || parser_check(parser, TOKEN_BANG)) {
        SrcLoc loc = parser_current_loc(parser);
        TokenKind op = parser_advance(parser)->kind;
        ASTNode *node = ast_new(parser->arena, NODE_UNARY, loc);
        node->unary.op = op;
        node->unary.operand = parse_unary(parser);
        return node;
    }
    // Deref: *expr
    if (parser_check(parser, TOKEN_STAR)) {
        SrcLoc loc = parser_current_loc(parser);
        parser_advance(parser); // consume '*'
        ASTNode *node = ast_new(parser->arena, NODE_DEREF, loc);
        node->deref.operand = parse_unary(parser);
        return node;
    }
    // Address-of / heap alloc: &expr
    if (parser_check(parser, TOKEN_AMPERSAND)) {
        SrcLoc loc = parser_current_loc(parser);
        parser_advance(parser); // consume '&'
        ASTNode *node = ast_new(parser->arena, NODE_ADDRESS_OF, loc);
        node->address_of.operand = parse_unary(parser);
        return node;
    }
    return parse_postfix(parser);
}

static ASTNode *parse_precedence(Parser *parser, Precedence minimum_precedence) {
    ASTNode *left = parse_unary(parser);

    for (;;) {
        TokenKind op = parser_current_token(parser)->kind;
        Precedence precedence = token_precedence(op);
        if (precedence < minimum_precedence) {
            break;
        }

        SrcLoc loc = parser_current_loc(parser);
        parser_advance(parser); // consume operator

        // Assignment
        if (op == TOKEN_EQUAL) {
            ASTNode *node = ast_new(parser->arena, NODE_ASSIGN, loc);
            node->assign.target = left;
            node->assign.value = parse_precedence(parser, precedence); // right-assoc
            left = node;
            continue;
        }

        // Compound assignment
        if (token_is_compound_assign(op)) {
            ASTNode *node = ast_new(parser->arena, NODE_COMPOUND_ASSIGN, loc);
            node->compound_assign.op = op;
            node->compound_assign.target = left;
            node->compound_assign.value = parse_precedence(parser, precedence);
            left = node;
            continue;
        }

        // Pipe operator: desugar a |> f into f(a), a |> f(b) into f(a, b)
        if (op == TOKEN_PIPE_GREATER) {
            ASTNode *rhs = parse_precedence(parser, precedence + 1);
            if (rhs->kind == NODE_CALL) {
                // a |> f(b, c) → f(a, b, c): prepend left to existing args
                int32_t old_count = BUF_LEN(rhs->call.args);
                ASTNode **new_args = NULL;
                const char **new_names = NULL;
                bool *new_mut = NULL;
                BUF_PUSH(new_args, left);
                BUF_PUSH(new_names, (const char *)NULL);
                BUF_PUSH(new_mut, false);
                for (int32_t i = 0; i < old_count; i++) {
                    BUF_PUSH(new_args, rhs->call.args[i]);
                    BUF_PUSH(new_names,
                             i < BUF_LEN(rhs->call.arg_names) ? rhs->call.arg_names[i] : NULL);
                    BUF_PUSH(new_mut,
                             i < BUF_LEN(rhs->call.arg_is_mut) ? rhs->call.arg_is_mut[i] : false);
                }
                rhs->call.args = new_args;
                rhs->call.arg_names = new_names;
                rhs->call.arg_is_mut = new_mut;
                left = rhs;
            } else {
                // a |> f → f(a)
                ASTNode *call = ast_new(parser->arena, NODE_CALL, loc);
                call->call.callee = rhs;
                call->call.args = NULL;
                call->call.arg_names = NULL;
                call->call.arg_is_mut = NULL;
                call->call.type_args = NULL;
                BUF_PUSH(call->call.args, left);
                left = call;
            }
            continue;
        }

        // Binary
        ASTNode *node = ast_new(parser->arena, NODE_BINARY, loc);
        node->binary.op = op;
        node->binary.left = left;
        node->binary.right = parse_precedence(parser, precedence + 1);
        left = node;
    }

    return left;
}

// ── If expr ──────────────────────────────────────────────────────

static ASTNode *parse_if(Parser *parser) {
    SrcLoc loc = parser_current_loc(parser);
    parser_expect(parser, TOKEN_IF);
    ASTNode *node = ast_new(parser->arena, NODE_IF, loc);

    bool saved = parser->no_struct_lit;
    parser->no_struct_lit = true;

    if (parser_is_pattern_binding(parser)) {
        node->if_expr.pattern = parser_parse_pattern(parser);
        parser_expect(parser, TOKEN_COLON_EQUAL);
        node->if_expr.pattern_init = parser_parse_expr(parser);
        node->if_expr.cond = NULL;
    } else {
        node->if_expr.cond = parser_parse_expr(parser);
        node->if_expr.pattern = NULL;
        node->if_expr.pattern_init = NULL;
    }

    parser->no_struct_lit = saved;

    node->if_expr.then_body = parser_parse_block(parser);
    node->if_expr.else_body = NULL;
    parser_skip_newlines(parser);
    if (parser_match(parser, TOKEN_ELSE)) {
        parser_skip_newlines(parser);
        if (parser_check(parser, TOKEN_IF)) {
            node->if_expr.else_body = parse_if(parser);
        } else {
            node->if_expr.else_body = parser_parse_block(parser);
        }
    }
    return node;
}

// ── Public entry point ─────────────────────────────────────────────────

ASTNode *parser_parse_expr(Parser *parser) {
    if (parser_check(parser, TOKEN_IF)) {
        return parse_if(parser);
    }
    if (parser_check(parser, TOKEN_MATCH)) {
        return parser_parse_match(parser);
    }
    return parse_precedence(parser, PRECEDENCE_ASSIGN);
}
