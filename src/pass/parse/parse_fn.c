/**
 * @file parse_fn.c
 * @brief Function, method, and generic parameter parsing.
 */

#include "_parse.h"

// ── Helpers ────────────────────────────────────────────────────────────

/** Peek one token ahead without consuming. */
bool parser_peek_is(const Parser *parser, TokenKind kind) {
    int32_t next = parser->pos + 1;
    if (next >= parser->count) {
        return false;
    }
    return parser->tokens[next].kind == kind;
}

// ── Generic type params / where clauses ──────────────────────────

/**
 * Parse generic type param list: `<T, U: Bound1 + Bound2, comptime N: usize, V = str>`.
 * Returns a stretchy buf of ASTTypeParam, or NULL if no `<` present.
 */
ASTTypeParam *parse_type_params(Parser *parser) {
    if (!parser_match(parser, TOKEN_LESS)) {
        return NULL;
    }
    ASTTypeParam *params = NULL;
    do {
        ASTTypeParam tp = {0};
        tp.bounds = NULL;
        tp.is_comptime = parser_match(parser, TOKEN_COMPTIME);
        tp.comptime_type = NULL;
        tp.default_type = NULL;
        tp.name = parser_expect(parser, TOKEN_ID)->lexeme;
        tp.assoc_constraints = NULL;
        if (parser_match(parser, TOKEN_COLON)) {
            if (tp.is_comptime) {
                // comptime N: usize — parse the type, not pact bounds
                tp.comptime_type = arena_alloc_zero(parser->arena, sizeof(ASTType));
                *tp.comptime_type = parser_parse_type(parser);
            } else {
                do {
                    const char *bound = parser_expect(parser, TOKEN_ID)->lexeme;
                    BUF_PUSH(tp.bounds, bound);
                    // Parse associated type constraints: Pact<Name = Type, ...>
                    if (parser_match(parser, TOKEN_LESS)) {
                        do {
                            ASTAssocConstraint ac = {0};
                            ac.pact_name = bound;
                            ac.assoc_name = parser_expect(parser, TOKEN_ID)->lexeme;
                            parser_expect(parser, TOKEN_EQUAL);
                            ac.expected_type = arena_alloc_zero(parser->arena, sizeof(ASTType));
                            *ac.expected_type = parser_parse_type(parser);
                            BUF_PUSH(tp.assoc_constraints, ac);
                        } while (parser_match(parser, TOKEN_COMMA));
                        parser_expect(parser, TOKEN_GREATER);
                    }
                } while (parser_match(parser, TOKEN_PLUS));
            }
        }
        if (parser_match(parser, TOKEN_EQUAL)) {
            tp.default_type = arena_alloc_zero(parser->arena, sizeof(ASTType));
            *tp.default_type = parser_parse_type(parser);
        }
        BUF_PUSH(params, tp);
    } while (parser_match(parser, TOKEN_COMMA));
    parser_expect(parser, TOKEN_GREATER);
    return params;
}

/**
 * Parse where clauses: `where T: Bound1 + Bound2, U: Bound3`.
 * Returns a stretchy buf of ASTWhereClause, or NULL if no `where` present.
 */
ASTWhereClause *parse_where_clauses(Parser *parser) {
    if (!parser_match(parser, TOKEN_WHERE)) {
        return NULL;
    }
    parser_skip_newlines(parser);
    ASTWhereClause *clauses = NULL;
    for (;;) {
        parser_skip_newlines(parser);
        ASTWhereClause wc = {0};
        wc.bounds = NULL;
        wc.assoc_member = NULL;
        wc.type_name = parser_expect(parser, TOKEN_ID)->lexeme;
        // Support projection: I::Item
        if (parser_match(parser, TOKEN_COLON_COLON)) {
            wc.assoc_member = parser_expect(parser, TOKEN_ID)->lexeme;
        }
        parser_expect(parser, TOKEN_COLON);
        do {
            const char *bound = parser_expect(parser, TOKEN_ID)->lexeme;
            BUF_PUSH(wc.bounds, bound);
        } while (parser_match(parser, TOKEN_PLUS));
        BUF_PUSH(clauses, wc);
        if (!parser_match(parser, TOKEN_COMMA)) {
            break;
        }
        parser_skip_newlines(parser);
        if (!parser_check(parser, TOKEN_ID)) {
            break;
        }
    }
    return clauses;
}

// ── Shared recv + params parsing ────────────────────────────────

/** Parse a single function parameter: [mut] name: [..]Type. */
static ASTNode *parse_single_param(Parser *parser, bool in_method) {
    SrcLoc ploc = parser_current_loc(parser);
    ASTNode *param = ast_new(parser->arena, NODE_PARAM, ploc);
    // Detect `*name` or `mut *name` receiver pattern in non-receiver position
    if (!in_method && (parser_check(parser, TOKEN_STAR) ||
                       (parser_check(parser, TOKEN_MUT) && parser_peek_is(parser, TOKEN_STAR)))) {
        PARSER_ERR(parser, ploc,
                   "receivers are only allowed as the first parameter of ext or pact methods");
        if (parser_match(parser, TOKEN_MUT)) {
            // consume 'mut'
        }
        parser_match(parser, TOKEN_STAR); // consume '*'
        param->param.name = parser_check(parser, TOKEN_ID) ? parser_advance(parser)->lexeme : "_";
        param->param.is_mut = false;
        param->param.is_variadic = false;
        param->param.type.kind = AST_TYPE_INFERRED;
        return param;
    }
    // Detect `*name` or `mut *name` receiver pattern in non-first method position
    if (in_method && (parser_check(parser, TOKEN_STAR) ||
                      (parser_check(parser, TOKEN_MUT) && parser_peek_is(parser, TOKEN_STAR)))) {
        PARSER_ERR(parser, ploc, "receiver must be the first parameter");
        // Skip the malformed receiver tokens to recover
        if (parser_match(parser, TOKEN_MUT)) {
            // consume 'mut'
        }
        parser_match(parser, TOKEN_STAR); // consume '*'
        param->param.name = parser_check(parser, TOKEN_ID) ? parser_advance(parser)->lexeme : "_";
        param->param.is_mut = false;
        param->param.is_variadic = false;
        param->param.type.kind = AST_TYPE_INFERRED;
        return param;
    }
    param->param.is_mut = parser_match(parser, TOKEN_MUT);
    param->param.is_variadic = false;
    param->param.name = parser_expect(parser, TOKEN_ID)->lexeme;
    // Detect untyped parameter (receiver-like) in non-receiver position
    if (!parser_check(parser, TOKEN_COLON)) {
        if (in_method) {
            PARSER_ERR(parser, ploc, "receiver must be the first parameter");
        } else {
            PARSER_ERR(parser, ploc,
                       "untyped parameter '%s'; "
                       "receivers are only allowed as the first parameter of ext or pact methods",
                       param->param.name);
        }
        // Produce a placeholder type to continue parsing
        param->param.type.kind = AST_TYPE_INFERRED;
        return param;
    }
    parser_expect(parser, TOKEN_COLON);
    // Detect variadic: `name: ..T`
    if (parser_match(parser, TOKEN_DOT_DOT)) {
        param->param.is_variadic = true;
    }
    param->param.type = parser_parse_type(parser);
    return param;
}

/** Parse trailing params (comma-separated) after receiver. */
static void parse_trailing_params(Parser *parser, ASTNode *node) {
    while (parser_match(parser, TOKEN_COMMA)) {
        ASTNode *param = parse_single_param(parser, true);
        BUF_PUSH(node->fn_decl.params, param);
    }
}

/**
 * Parse method receiver and params between `(` and `)`.
 *
 * Recognizes three receiver forms:
 *   - `*name`      → ptr recv (read-only)
 *   - `mut *name`  → ptr recv (mutable)
 *   - `name`       → value recv (id not followed by `:`)
 *
 * Falls through to regular param parsing when no receiver is present.
 */
void parse_recv_and_params(Parser *parser, ASTNode *node) {
    parser_expect(parser, TOKEN_LEFT_PAREN);
    if (!parser_check(parser, TOKEN_RIGHT_PAREN)) {
        if (parser_check(parser, TOKEN_STAR) || parser_check(parser, TOKEN_MUT)) {
            bool is_mut = false;
            if (parser_match(parser, TOKEN_MUT)) {
                is_mut = true;
            }
            parser_expect(parser, TOKEN_STAR);
            node->fn_decl.recv_name = parser_expect(parser, TOKEN_ID)->lexeme;
            node->fn_decl.is_mut_recv = is_mut;
            node->fn_decl.is_ptr_recv = true;
            parse_trailing_params(parser, node);
        } else if (parser_check(parser, TOKEN_ID) && !parser_peek_is(parser, TOKEN_COLON)) {
            node->fn_decl.recv_name = parser_expect(parser, TOKEN_ID)->lexeme;
            node->fn_decl.is_ptr_recv = false;
            parse_trailing_params(parser, node);
        } else {
            do {
                ASTNode *param = parse_single_param(parser, true);
                BUF_PUSH(node->fn_decl.params, param);
            } while (parser_match(parser, TOKEN_COMMA));
        }
    }
    parser_expect(parser, TOKEN_RIGHT_PAREN);

    // Validate: variadic param must be last
    int32_t param_count = BUF_LEN(node->fn_decl.params);
    for (int32_t i = 0; i < param_count; i++) {
        ASTNode *p = node->fn_decl.params[i];
        if (p->param.is_variadic && i != param_count - 1) {
            PARSER_ERR(parser, p->loc, "variadic must be last parameter");
        }
    }
}

// ── Method decl (inside struct/enum/ext) ────────────────────────

ASTNode *parse_method_decl(Parser *parser, const char *struct_name, bool is_pub) {
    bool is_declare = parser_match(parser, TOKEN_DECLARE);
    SrcLoc loc = parser_current_loc(parser);
    parser_expect(parser, TOKEN_FN);

    ASTNode *node = ast_new(parser->arena, NODE_FN_DECL, loc);
    node->fn_decl.is_pub = is_pub;
    node->fn_decl.is_declare = is_declare;
    node->fn_decl.name = parser_expect(parser, TOKEN_ID)->lexeme;
    node->fn_decl.params = NULL;
    node->fn_decl.owner_struct = struct_name;
    node->fn_decl.recv_name = NULL;
    node->fn_decl.is_mut_recv = false;
    node->fn_decl.is_ptr_recv = false;

    parse_recv_and_params(parser, node);

    // Return type
    node->fn_decl.return_type.kind = AST_TYPE_INFERRED;
    if (parser_match(parser, TOKEN_ARROW)) {
        node->fn_decl.return_type = parser_parse_type(parser);
    }

    parser_skip_newlines(parser);

    // Body: block or = expr (absent for decl methods)
    if (is_declare) {
        node->fn_decl.body = NULL;
    } else if (parser_check(parser, TOKEN_LEFT_BRACE)) {
        node->fn_decl.body = parser_parse_block(parser);
    } else if (parser_match(parser, TOKEN_EQUAL)) {
        node->fn_decl.body = parser_parse_expr(parser);
    } else {
        PARSER_ERR(parser, parser_current_loc(parser), "expected method body");
    }

    return node;
}

// ── Fn decl (top-level) ─────────────────────────────────────────

/** Parse `decl var name: Type` — variable declaration without initializer. */
ASTNode *parse_declare_var(Parser *parser, bool is_pub) {
    SrcLoc loc = parser_current_loc(parser);
    parser_expect(parser, TOKEN_VAR);

    ASTNode *node = ast_new(parser->arena, NODE_VAR_DECL, loc);
    node->var_decl.name = parser_expect(parser, TOKEN_ID)->lexeme;
    parser_expect(parser, TOKEN_COLON);
    node->var_decl.type = parser_parse_type(parser);
    node->var_decl.init = NULL;
    node->var_decl.is_var = true;
    node->var_decl.is_immut = false;
    node->var_decl.is_declare = true;
    node->var_decl.is_pub = is_pub;
    return node;
}

ASTNode *parse_fn_decl(Parser *parser, bool is_pub) {
    bool is_declare = parser_match(parser, TOKEN_DECLARE);
    SrcLoc loc = parser_current_loc(parser);
    parser_expect(parser, TOKEN_FN);

    ASTNode *node = ast_new(parser->arena, NODE_FN_DECL, loc);
    node->fn_decl.is_pub = is_pub;
    node->fn_decl.is_declare = is_declare;
    node->fn_decl.name = parser_expect(parser, TOKEN_ID)->lexeme;
    node->fn_decl.params = NULL;
    node->fn_decl.recv_name = NULL;
    node->fn_decl.is_mut_recv = false;
    node->fn_decl.is_ptr_recv = false;
    node->fn_decl.owner_struct = NULL;
    node->fn_decl.where_clauses = NULL;
    node->fn_decl.type_params = parse_type_params(parser);

    // Parameters
    parser_expect(parser, TOKEN_LEFT_PAREN);
    if (!parser_check(parser, TOKEN_RIGHT_PAREN)) {
        do {
            ASTNode *param = parse_single_param(parser, false);
            BUF_PUSH(node->fn_decl.params, param);
        } while (parser_match(parser, TOKEN_COMMA));
    }
    parser_expect(parser, TOKEN_RIGHT_PAREN);

    // Validate: variadic param must be last
    {
        int32_t param_count = BUF_LEN(node->fn_decl.params);
        for (int32_t i = 0; i < param_count; i++) {
            ASTNode *p = node->fn_decl.params[i];
            if (p->param.is_variadic && i != param_count - 1) {
                PARSER_ERR(parser, p->loc, "variadic must be last parameter");
            }
        }
    }

    // Return type
    node->fn_decl.return_type.kind = AST_TYPE_INFERRED;
    if (parser_match(parser, TOKEN_ARROW)) {
        node->fn_decl.return_type = parser_parse_type(parser);
    }

    // Optional where clauses — use skip_lines_only so doc comments on the NEXT
    // top-level declaration are not silently consumed.
    parser_skip_lines_only(parser);
    node->fn_decl.where_clauses = parse_where_clauses(parser);

    parser_skip_lines_only(parser);

    // Body: block or `= expr` (absent for decl fns)
    if (is_declare) {
        node->fn_decl.body = NULL;
    } else if (parser_check(parser, TOKEN_LEFT_BRACE)) {
        node->fn_decl.body = parser_parse_block(parser);
    } else if (parser_match(parser, TOKEN_EQUAL)) {
        node->fn_decl.body = parser_parse_expr(parser);
    } else {
        PARSER_ERR(parser, parser_current_loc(parser), "expected fn body");
    }

    return node;
}
