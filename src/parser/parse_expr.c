#include "_parser.h"

// ── Operator classification ─────────────────────────────────────────────

static bool is_compound_assignment_op(TokenKind op) {
    return op == TOKEN_PLUS_EQUAL || op == TOKEN_MINUS_EQUAL || op == TOKEN_STAR_EQUAL ||
           op == TOKEN_SLASH_EQUAL;
}

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

static Precedence token_precedence(TokenKind kind) {
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

static ASTNode *parse_str_interpolation(Parser *parser, SourceLoc loc) {
    ASTNode *interpolation = ast_new(parser->arena, NODE_STR_INTERPOLATION, loc);
    interpolation->str_interpolation.parts = NULL;

    ASTNode *text = ast_new(parser->arena, NODE_LIT, loc);
    text->lit.kind = LIT_STR;
    text->lit.str_value = parser_previous_token(parser)->lit_value.str_value;
    BUF_PUSH(interpolation->str_interpolation.parts, text);

    while (parser_match(parser, TOKEN_INTERPOLATION_START)) {
        ASTNode *expr = parser_parse_expr(parser);
        BUF_PUSH(interpolation->str_interpolation.parts, expr);
        parser_expect(parser, TOKEN_INTERPOLATION_END);

        SourceLoc text_loc = parser_current_loc(parser);
        parser_expect(parser, TOKEN_STR_LIT);
        ASTNode *text2 = ast_new(parser->arena, NODE_LIT, text_loc);
        text2->lit.kind = LIT_STR;
        text2->lit.str_value = parser_previous_token(parser)->lit_value.str_value;
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
    SourceLoc loc = name_node->loc;
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
    SourceLoc loc = parser_current_loc(parser);
    parser_expect(parser, TOKEN_LEFT_BRACKET);

    // Check for slice lit: []T{values}
    // Pattern: ']', type-keyword-or-id, '{'
    if (parser_check(parser, TOKEN_RIGHT_BRACKET)) {
        int32_t save_pos = parser->pos;
        parser_advance(parser); // consume ']'
        if (token_is_type_keyword(parser_current_token(parser)->kind) ||
            parser_check(parser, TOKEN_ID) || parser_check(parser, TOKEN_LEFT_BRACKET)) {
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
                           parser->tokens[parser->pos + 2].kind == TOKEN_ID);
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

static ASTNode *parse_primary(Parser *parser) {
    SourceLoc loc = parser_current_loc(parser);

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

    if (parser_match(parser, TOKEN_ID)) {
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
            parser->err_count++;
            rsg_err(loc, "empty tuple '()' is not allowed; use 'unit'");
            ASTNode *node = ast_new(parser->arena, NODE_LIT, loc);
            node->lit.kind = LIT_UNIT;
            return node;
        }
        ASTNode *first = parser_parse_expr(parser);
        if (parser_match(parser, TOKEN_COMMA)) {
            // Tuple lit: (expr, expr, ...)
            ASTNode *node = ast_new(parser->arena, NODE_TUPLE_LIT, loc);
            node->tuple_lit.elems = NULL;
            BUF_PUSH(node->tuple_lit.elems, first);
            parse_comma_separated(parser, &node->tuple_lit.elems);
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
    const char *token_name = token_kind_str(parser_current_token(parser)->kind);
    rsg_err(loc, "expected expr, got '%s'", token_name);
    parser_advance(parser);
    return ast_new(parser->arena, NODE_LIT, loc); // err recovery
}

// ── Postfix / unary / precedence climbing ──────────────────────────────

/** Parse a comma-separated arg list (posal, named, and mut). */
static ASTNode *parse_call_args(Parser *parser, ASTNode *callee, SourceLoc loc) {
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

static ASTNode *parse_postfix(Parser *parser) {
    ASTNode *left = parse_primary(parser);
    for (;;) {
        SourceLoc loc = parser_current_loc(parser);

        // Struct lit: Identifier { field = expr, ... }
        if (left->kind == NODE_ID && is_struct_lit_ahead(parser)) {
            left = parse_struct_lit(parser, left);
            continue;
        }

        // Enum struct variant lit: Enum::Variant { field = expr, ... }
        if (left->kind == NODE_MEMBER && left->member.object->kind == NODE_ID &&
            is_struct_lit_ahead(parser)) {
            left = parse_enum_struct_lit(parser, left);
            continue;
        }

        if (parser_match(parser, TOKEN_LEFT_PAREN)) {
            left = parse_call_args(parser, left, loc);
            continue;
        }
        if (parser_match(parser, TOKEN_LEFT_BRACKET)) {
            // Check for slice exprs: obj[..], obj[start..end], obj[start..], obj[..end]
            if (parser_check(parser, TOKEN_DOT_DOT)) {
                // obj[..] or obj[..end]
                parser_advance(parser); // consume '..'
                ASTNode *node = ast_new(parser->arena, NODE_SLICE_EXPR, loc);
                node->slice_expr.object = left;
                node->slice_expr.start = NULL;
                if (parser_check(parser, TOKEN_RIGHT_BRACKET)) {
                    node->slice_expr.end = NULL;
                    node->slice_expr.full_range = true;
                } else {
                    node->slice_expr.end = parser_parse_expr(parser);
                    node->slice_expr.full_range = false;
                }
                parser_expect(parser, TOKEN_RIGHT_BRACKET);
                left = node;
                continue;
            }
            // Parse idx expr; check if it becomes a slice expr
            ASTNode *idx_or_start = parser_parse_expr(parser);
            if (parser_check(parser, TOKEN_DOT_DOT)) {
                // obj[start..end] or obj[start..]
                parser_advance(parser); // consume '..'
                ASTNode *node = ast_new(parser->arena, NODE_SLICE_EXPR, loc);
                node->slice_expr.object = left;
                node->slice_expr.start = idx_or_start;
                node->slice_expr.full_range = false;
                if (parser_check(parser, TOKEN_RIGHT_BRACKET)) {
                    node->slice_expr.end = NULL;
                } else {
                    node->slice_expr.end = parser_parse_expr(parser);
                }
                parser_expect(parser, TOKEN_RIGHT_BRACKET);
                left = node;
                continue;
            }
            // Regular idx access: obj[idx]
            ASTNode *node = ast_new(parser->arena, NODE_IDX, loc);
            node->idx_access.object = left;
            node->idx_access.idx = idx_or_start;
            parser_expect(parser, TOKEN_RIGHT_BRACKET);
            left = node;
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
        break;
    }
    return left;
}

static ASTNode *parse_unary(Parser *parser) {
    if (parser_check(parser, TOKEN_MINUS) || parser_check(parser, TOKEN_BANG)) {
        SourceLoc loc = parser_current_loc(parser);
        TokenKind op = parser_advance(parser)->kind;
        ASTNode *node = ast_new(parser->arena, NODE_UNARY, loc);
        node->unary.op = op;
        node->unary.operand = parse_unary(parser);
        return node;
    }
    // Deref: *expr
    if (parser_check(parser, TOKEN_STAR)) {
        SourceLoc loc = parser_current_loc(parser);
        parser_advance(parser); // consume '*'
        ASTNode *node = ast_new(parser->arena, NODE_DEREF, loc);
        node->deref.operand = parse_unary(parser);
        return node;
    }
    // Address-of / heap alloc: &expr
    if (parser_check(parser, TOKEN_AMPERSAND)) {
        SourceLoc loc = parser_current_loc(parser);
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

        SourceLoc loc = parser_current_loc(parser);
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
        if (is_compound_assignment_op(op)) {
            ASTNode *node = ast_new(parser->arena, NODE_COMPOUND_ASSIGN, loc);
            node->compound_assign.op = op;
            node->compound_assign.target = left;
            node->compound_assign.value = parse_precedence(parser, precedence);
            left = node;
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

// ── Match expr ───────────────────────────────────────────────────

static ASTNode *parse_match(Parser *parser) {
    SourceLoc loc = parser_current_loc(parser);
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

// ── If expr ──────────────────────────────────────────────────────

static ASTNode *parse_if(Parser *parser) {
    SourceLoc loc = parser_current_loc(parser);
    parser_expect(parser, TOKEN_IF);
    ASTNode *node = ast_new(parser->arena, NODE_IF, loc);
    node->if_expr.cond = parser_parse_expr(parser);
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
        return parse_match(parser);
    }
    return parse_precedence(parser, PRECEDENCE_ASSIGN);
}
