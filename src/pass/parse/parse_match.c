#include "_parse.h"

// ── Pattern parsing ────────────────────────────────────────────────────

static ASTPattern *parse_pattern(Parser *parser) {
    ASTPattern *pattern = arena_alloc_zero(parser->arena, sizeof(ASTPattern));
    pattern->loc = parser_current_loc(parser);
    pattern->sub_patterns = NULL;
    pattern->field_names = NULL;

    // Wildcard: _
    if (parser_check(parser, TOKEN_ID) && parser_current_token(parser)->lexeme[0] == '_' &&
        parser_current_token(parser)->lexeme[1] == '\0') {
        parser_advance(parser); // consume '_'
        pattern->kind = PATTERN_WILDCARD;
        return pattern;
    }

    // unit keyword → unit literal pattern
    if (parser_match(parser, TOKEN_UNIT)) {
        pattern->kind = PATTERN_LIT;
        pattern->lit = ast_new(parser->arena, NODE_LIT, pattern->loc);
        pattern->lit->lit.kind = LIT_UNIT;
        return pattern;
    }

    // Empty tuple () → unit literal pattern
    if (parser_check(parser, TOKEN_LEFT_PAREN)) {
        SrcLoc paren_loc = parser_current_loc(parser);
        parser_advance(parser); // consume '('
        if (parser_match(parser, TOKEN_RIGHT_PAREN)) {
            pattern->kind = PATTERN_LIT;
            pattern->lit = ast_new(parser->arena, NODE_LIT, paren_loc);
            pattern->lit->lit.kind = LIT_UNIT;
            return pattern;
        }
        // Not an empty tuple — rewind is not possible, so error
        rsg_err(paren_loc, "expected ')' for unit pattern or use a different pattern form");
        pattern->kind = PATTERN_WILDCARD;
        return pattern;
    }

    // Boolean lits
    if (parser_match(parser, TOKEN_TRUE)) {
        pattern->kind = PATTERN_LIT;
        pattern->lit = ast_new(parser->arena, NODE_LIT, pattern->loc);
        pattern->lit->lit.kind = LIT_BOOL;
        pattern->lit->lit.boolean_value = true;
        return pattern;
    }
    if (parser_match(parser, TOKEN_FALSE)) {
        pattern->kind = PATTERN_LIT;
        pattern->lit = ast_new(parser->arena, NODE_LIT, pattern->loc);
        pattern->lit->lit.kind = LIT_BOOL;
        pattern->lit->lit.boolean_value = false;
        return pattern;
    }

    // Str lit
    if (parser_match(parser, TOKEN_STR_LIT)) {
        pattern->kind = PATTERN_LIT;
        pattern->lit = ast_new(parser->arena, NODE_LIT, pattern->loc);
        pattern->lit->lit.kind = LIT_STR;
        pattern->lit->lit.str_value = parser_previous_token(parser)->lit_value.str_value;
        pattern->lit->lit.str_len = parser_previous_token(parser)->len;
        return pattern;
    }

    // Integer lit (possibly followed by .. or ..= for range)
    if (parser_match(parser, TOKEN_INTEGER_LIT)) {
        ASTNode *start = ast_new(parser->arena, NODE_LIT, pattern->loc);
        start->lit.kind = LIT_I32;
        start->lit.integer_value = parser_previous_token(parser)->lit_value.integer_value;

        // Range pattern: N..M or N..=M
        if (parser_check(parser, TOKEN_DOT_DOT) || parser_check(parser, TOKEN_DOT_DOT_EQUAL)) {
            pattern->kind = PATTERN_RANGE;
            pattern->range_inclusive = parser_current_token(parser)->kind == TOKEN_DOT_DOT_EQUAL;
            parser_advance(parser); // consume '..' or '..='
            pattern->range_start = start;
            ASTNode *end = ast_new(parser->arena, NODE_LIT, parser_current_loc(parser));
            end->lit.kind = LIT_I32;
            end->lit.integer_value =
                parser_expect(parser, TOKEN_INTEGER_LIT)->lit_value.integer_value;
            pattern->range_end = end;
            return pattern;
        }

        pattern->kind = PATTERN_LIT;
        pattern->lit = start;
        return pattern;
    }

    // Negative integer lit
    if (parser_match(parser, TOKEN_MINUS)) {
        if (parser_match(parser, TOKEN_INTEGER_LIT)) {
            ASTNode *lit = ast_new(parser->arena, NODE_LIT, pattern->loc);
            lit->lit.kind = LIT_I32;
            lit->lit.integer_value =
                (uint64_t)(-(int64_t)parser_previous_token(parser)->lit_value.integer_value);
            pattern->kind = PATTERN_LIT;
            pattern->lit = lit;
            return pattern;
        }
        rsg_err(pattern->loc, "expected integer after '-' in pattern");
    }

    // Float lit
    if (parser_match(parser, TOKEN_FLOAT_LIT)) {
        pattern->kind = PATTERN_LIT;
        pattern->lit = ast_new(parser->arena, NODE_LIT, pattern->loc);
        pattern->lit->lit.kind = LIT_F64;
        pattern->lit->lit.float64_value = parser_previous_token(parser)->lit_value.float_value;
        return pattern;
    }

    // Identifier: binding, variant_unit, variant_tuple, or variant_struct
    if (parser_match(parser, TOKEN_ID)) {
        pattern->name = parser_previous_token(parser)->lexeme;

        // Tuple variant pattern: Ident(sub, sub, ...)
        if (parser_match(parser, TOKEN_LEFT_PAREN)) {
            pattern->kind = PATTERN_VARIANT_TUPLE;
            if (!parser_check(parser, TOKEN_RIGHT_PAREN)) {
                do {
                    ASTPattern *sub = parse_pattern(parser);
                    BUF_PUSH(pattern->sub_patterns, sub);
                } while (parser_match(parser, TOKEN_COMMA));
            }
            parser_expect(parser, TOKEN_RIGHT_PAREN);
            return pattern;
        }

        // Struct variant pattern: Ident { field, field, ... }
        if (parser_check(parser, TOKEN_LEFT_BRACE)) {
            pattern->kind = PATTERN_VARIANT_STRUCT;
            parser_expect(parser, TOKEN_LEFT_BRACE);
            parser_skip_newlines(parser);
            if (!parser_check(parser, TOKEN_RIGHT_BRACE)) {
                do {
                    parser_skip_newlines(parser);
                    const char *field_name = parser_expect(parser, TOKEN_ID)->lexeme;
                    BUF_PUSH(pattern->field_names, field_name);
                } while (parser_match(parser, TOKEN_COMMA));
            }
            parser_skip_newlines(parser);
            parser_expect(parser, TOKEN_RIGHT_BRACE);
            return pattern;
        }

        // Plain id: binding (sema resolves to variant_unit if matching)
        pattern->kind = PATTERN_BINDING;
        return pattern;
    }

    rsg_err(pattern->loc, "expected pattern");
    pattern->kind = PATTERN_WILDCARD;
    return pattern;
}

ASTPattern *parser_parse_pattern(Parser *parser) {
    return parse_pattern(parser);
}

// ── Match expr ───────────────────────────────────────────────────

ASTNode *parser_parse_match(Parser *parser) {
    SrcLoc loc = parser_current_loc(parser);
    parser_expect(parser, TOKEN_MATCH);

    ASTNode *node = ast_new(parser->arena, NODE_MATCH, loc);
    node->match_expr.operand = parser_parse_expr(parser);
    node->match_expr.arms = NULL;

    parser_skip_newlines(parser);
    parser_expect(parser, TOKEN_LEFT_BRACE);
    parser_skip_newlines(parser);

    while (!parser_check(parser, TOKEN_RIGHT_BRACE) && !parser_at_end(parser)) {
        ASTMatchArm arm = {0};
        arm.pattern = parse_pattern(parser);

        // Optional guard: if cond
        arm.guard = NULL;
        if (parser_match(parser, TOKEN_IF)) {
            arm.guard = parser_parse_expr(parser);
        }

        parser_expect(parser, TOKEN_FAT_ARROW);
        arm.body = parser_parse_expr(parser);

        BUF_PUSH(node->match_expr.arms, arm);

        // Consume optional comma and newlines between arms
        parser_match(parser, TOKEN_COMMA);
        parser_skip_newlines(parser);
    }

    parser_expect(parser, TOKEN_RIGHT_BRACE);
    return node;
}
