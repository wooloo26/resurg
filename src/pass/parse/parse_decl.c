#include "_parse.h"

// ── Forward declarations ──────────────────────────────────────────────
static ASTTypeParam *parse_type_params(Parser *parser);

// ── Helpers ────────────────────────────────────────────────────────────

/** Peek one token ahead without consuming. */
static bool parser_peek_is(const Parser *parser, TokenKind kind) {
    int32_t next = parser->pos + 1;
    if (next >= parser->count) {
        return false;
    }
    return parser->tokens[next].kind == kind;
}

// ── Shared recv + params parsing ────────────────────────────────

/** Parse trailing params (comma-separated `name: Type`) after receiver. */
static void parse_trailing_params(Parser *parser, ASTNode *node) {
    while (parser_match(parser, TOKEN_COMMA)) {
        SrcLoc ploc = parser_current_loc(parser);
        ASTNode *param = ast_new(parser->arena, NODE_PARAM, ploc);
        param->param.name = parser_expect(parser, TOKEN_ID)->lexeme;
        parser_expect(parser, TOKEN_COLON);
        param->param.type = parser_parse_type(parser);
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
static void parse_recv_and_params(Parser *parser, ASTNode *node) {
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
                SrcLoc ploc = parser_current_loc(parser);
                ASTNode *param = ast_new(parser->arena, NODE_PARAM, ploc);
                param->param.name = parser_expect(parser, TOKEN_ID)->lexeme;
                parser_expect(parser, TOKEN_COLON);
                param->param.type = parser_parse_type(parser);
                BUF_PUSH(node->fn_decl.params, param);
            } while (parser_match(parser, TOKEN_COMMA));
        }
    }
    parser_expect(parser, TOKEN_RIGHT_PAREN);
}

// ── Method decl (inside struct/enum) ────────────────────────────

static ASTNode *parse_method_decl(Parser *parser, const char *struct_name) {
    SrcLoc loc = parser_current_loc(parser);
    parser_expect(parser, TOKEN_FN);

    ASTNode *node = ast_new(parser->arena, NODE_FN_DECL, loc);
    node->fn_decl.is_pub = false;
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

    // Body: block or = expr
    if (parser_check(parser, TOKEN_LEFT_BRACE)) {
        node->fn_decl.body = parser_parse_block(parser);
    } else if (parser_match(parser, TOKEN_EQUAL)) {
        node->fn_decl.body = parser_parse_expr(parser);
    } else {
        rsg_err(parser_current_loc(parser), "expected method body");
    }

    return node;
}

// ── Pact method decl (body may be absent for required methods) ──

static ASTNode *parse_pact_method(Parser *parser, const char *pact_name) {
    SrcLoc loc = parser_current_loc(parser);
    parser_expect(parser, TOKEN_FN);

    ASTNode *node = ast_new(parser->arena, NODE_FN_DECL, loc);
    node->fn_decl.is_pub = false;
    node->fn_decl.name = parser_expect(parser, TOKEN_ID)->lexeme;
    node->fn_decl.params = NULL;
    node->fn_decl.owner_struct = pact_name;
    node->fn_decl.recv_name = NULL;
    node->fn_decl.is_mut_recv = false;
    node->fn_decl.is_ptr_recv = false;

    parse_recv_and_params(parser, node);

    node->fn_decl.return_type.kind = AST_TYPE_INFERRED;
    if (parser_match(parser, TOKEN_ARROW)) {
        node->fn_decl.return_type = parser_parse_type(parser);
    }

    parser_skip_newlines(parser);

    // Body is optional for pact methods (absent = required method)
    if (parser_check(parser, TOKEN_LEFT_BRACE)) {
        node->fn_decl.body = parser_parse_block(parser);
    } else if (parser_match(parser, TOKEN_EQUAL)) {
        node->fn_decl.body = parser_parse_expr(parser);
    } else {
        node->fn_decl.body = NULL;
    }

    return node;
}

// ── Pact decl ───────────────────────────────────────────────────

static ASTNode *parse_pact_decl(Parser *parser) {
    SrcLoc loc = parser_current_loc(parser);
    parser_expect(parser, TOKEN_PACT);

    ASTNode *node = ast_new(parser->arena, NODE_PACT_DECL, loc);
    node->pact_decl.name = parser_expect(parser, TOKEN_ID)->lexeme;
    node->pact_decl.fields = NULL;
    node->pact_decl.methods = NULL;
    node->pact_decl.super_pacts = NULL;
    node->pact_decl.type_params = parse_type_params(parser);

    parser_skip_newlines(parser);

    // Shorthand constraint alias: pact A = B + C
    if (parser_match(parser, TOKEN_EQUAL)) {
        do {
            const char *pact_name = parser_expect(parser, TOKEN_ID)->lexeme;
            BUF_PUSH(node->pact_decl.super_pacts, pact_name);
        } while (parser_match(parser, TOKEN_PLUS));
        return node;
    }

    parser_expect(parser, TOKEN_LEFT_BRACE);
    parser_skip_newlines(parser);

    while (!parser_check(parser, TOKEN_RIGHT_BRACE) && !parser_at_end(parser)) {
        if (parser_check(parser, TOKEN_FN)) {
            ASTNode *method = parse_pact_method(parser, node->pact_decl.name);
            BUF_PUSH(node->pact_decl.methods, method);
        } else if (parser_check(parser, TOKEN_ID)) {
            const char *name = parser_advance(parser)->lexeme;

            if (parser_match(parser, TOKEN_COLON)) {
                // Required field: name: Type
                ASTStructField field = {0};
                field.name = name;
                field.type = parser_parse_type(parser);
                field.default_value = NULL;
                BUF_PUSH(node->pact_decl.fields, field);
            } else {
                // Super pact ref (constraint alias brace syntax)
                BUF_PUSH(node->pact_decl.super_pacts, name);
                // Consume optional generic type args: Into<T>
                if (parser_match(parser, TOKEN_LESS)) {
                    do {
                        parser_parse_type(parser);
                    } while (parser_match(parser, TOKEN_COMMA));
                    parser_expect(parser, TOKEN_GREATER);
                }
            }
        } else {
            rsg_err(parser_current_loc(parser),
                    "expected field, method, or pact name in pact decl");
            parser_advance(parser);
        }
        parser_skip_newlines(parser);
    }

    parser_expect(parser, TOKEN_RIGHT_BRACE);
    return node;
}

// ── Enum decl ───────────────────────────────────────────────────

static ASTNode *parse_enum_decl(Parser *parser) {
    SrcLoc loc = parser_current_loc(parser);
    parser_expect(parser, TOKEN_ENUM);

    ASTNode *node = ast_new(parser->arena, NODE_ENUM_DECL, loc);
    node->enum_decl.name = parser_expect(parser, TOKEN_ID)->lexeme;
    node->enum_decl.variants = NULL;
    node->enum_decl.methods = NULL;

    parser_skip_newlines(parser);
    parser_expect(parser, TOKEN_LEFT_BRACE);
    parser_skip_newlines(parser);

    while (!parser_check(parser, TOKEN_RIGHT_BRACE) && !parser_at_end(parser)) {
        if (parser_check(parser, TOKEN_FN)) {
            // Method decl inside enum
            ASTNode *method = parse_method_decl(parser, node->enum_decl.name);
            BUF_PUSH(node->enum_decl.methods, method);
        } else if (parser_check(parser, TOKEN_ID)) {
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
                        BUF_PUSH(variant.tuple_types, *elem);
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
                        field.name = parser_expect(parser, TOKEN_ID)->lexeme;
                        parser_expect(parser, TOKEN_COLON);
                        field.type = parser_parse_type(parser);
                        field.default_value = NULL;
                        BUF_PUSH(variant.fields, field);
                    } while (parser_match(parser, TOKEN_COMMA));
                }
                parser_skip_newlines(parser);
                parser_expect(parser, TOKEN_RIGHT_BRACE);
            } else if (parser_match(parser, TOKEN_EQUAL)) {
                // Explicit discriminant: Name = expr
                variant.discriminant = parser_parse_expr(parser);
            }

            BUF_PUSH(node->enum_decl.variants, variant);
        } else {
            rsg_err(parser_current_loc(parser), "expected variant or method in enum");
            parser_advance(parser);
        }
        // Consume optional comma and newlines between variants
        parser_match(parser, TOKEN_COMMA);
        parser_skip_newlines(parser);
    }

    parser_expect(parser, TOKEN_RIGHT_BRACE);
    return node;
}

// ── Struct decl ─────────────────────────────────────────────────

static ASTNode *parse_struct_decl(Parser *parser) {
    SrcLoc loc = parser_current_loc(parser);
    parser_expect(parser, TOKEN_STRUCT);

    ASTNode *node = ast_new(parser->arena, NODE_STRUCT_DECL, loc);
    node->struct_decl.name = parser_expect(parser, TOKEN_ID)->lexeme;
    node->struct_decl.fields = NULL;
    node->struct_decl.methods = NULL;
    node->struct_decl.embedded = NULL;
    node->struct_decl.conformances = NULL;

    // Parse optional conformance list: struct Foo: Pact1 + Pact2 + Into<str>
    if (parser_match(parser, TOKEN_COLON)) {
        do {
            parser_skip_newlines(parser);
            const char *pact_name = parser_expect(parser, TOKEN_ID)->lexeme;
            BUF_PUSH(node->struct_decl.conformances, pact_name);
            // Consume optional generic type args: Into<str>
            if (parser_match(parser, TOKEN_LESS)) {
                do {
                    parser_parse_type(parser);
                } while (parser_match(parser, TOKEN_COMMA));
                parser_expect(parser, TOKEN_GREATER);
            }
        } while (parser_match(parser, TOKEN_PLUS));
    }

    parser_skip_newlines(parser);
    parser_expect(parser, TOKEN_LEFT_BRACE);
    parser_skip_newlines(parser);

    while (!parser_check(parser, TOKEN_RIGHT_BRACE) && !parser_at_end(parser)) {
        if (parser_check(parser, TOKEN_FN)) {
            // Method decl
            ASTNode *method = parse_method_decl(parser, node->struct_decl.name);
            BUF_PUSH(node->struct_decl.methods, method);
        } else if (parser_check(parser, TOKEN_ID)) {
            const char *name = parser_advance(parser)->lexeme;

            if (parser_match(parser, TOKEN_COLON)) {
                // Field: name: Type [= default]
                ASTStructField field = {0};
                field.name = name;
                field.type = parser_parse_type(parser);
                field.default_value = NULL;
                if (parser_match(parser, TOKEN_EQUAL)) {
                    field.default_value = parser_parse_expr(parser);
                }
                BUF_PUSH(node->struct_decl.fields, field);
            } else {
                // Embedded struct: just a type name on its own line
                BUF_PUSH(node->struct_decl.embedded, name);
            }
        } else {
            rsg_err(parser_current_loc(parser), "expected field, method, or embedded struct");
            parser_advance(parser);
        }
        parser_skip_newlines(parser);
    }

    parser_expect(parser, TOKEN_RIGHT_BRACE);
    return node;
}

// ── Fn decl ───────────────────────────────────────────────

/**
 * Parse generic type param list: `<T, U: Bound1 + Bound2, ...>`.
 * Returns a stretchy buf of ASTTypeParam, or NULL if no `<` present.
 */
static ASTTypeParam *parse_type_params(Parser *parser) {
    if (!parser_match(parser, TOKEN_LESS)) {
        return NULL;
    }
    ASTTypeParam *params = NULL;
    do {
        ASTTypeParam tp = {0};
        tp.name = parser_expect(parser, TOKEN_ID)->lexeme;
        tp.bounds = NULL;
        if (parser_match(parser, TOKEN_COLON)) {
            do {
                const char *bound = parser_expect(parser, TOKEN_ID)->lexeme;
                BUF_PUSH(tp.bounds, bound);
            } while (parser_match(parser, TOKEN_PLUS));
        }
        BUF_PUSH(params, tp);
    } while (parser_match(parser, TOKEN_COMMA));
    parser_expect(parser, TOKEN_GREATER);
    return params;
}

static ASTNode *parse_fn_decl(Parser *parser, bool is_pub) {
    SrcLoc loc = parser_current_loc(parser);
    parser_expect(parser, TOKEN_FN);

    ASTNode *node = ast_new(parser->arena, NODE_FN_DECL, loc);
    node->fn_decl.is_pub = is_pub;
    node->fn_decl.name = parser_expect(parser, TOKEN_ID)->lexeme;
    node->fn_decl.params = NULL;
    node->fn_decl.recv_name = NULL;
    node->fn_decl.is_mut_recv = false;
    node->fn_decl.is_ptr_recv = false;
    node->fn_decl.owner_struct = NULL;
    node->fn_decl.type_params = parse_type_params(parser);

    // Parameters
    parser_expect(parser, TOKEN_LEFT_PAREN);
    if (!parser_check(parser, TOKEN_RIGHT_PAREN)) {
        do {
            SrcLoc param_loc = parser_current_loc(parser);
            ASTNode *param = ast_new(parser->arena, NODE_PARAM, param_loc);
            param->param.is_mut = parser_match(parser, TOKEN_MUT);
            param->param.name = parser_expect(parser, TOKEN_ID)->lexeme;
            parser_expect(parser, TOKEN_COLON);
            param->param.type = parser_parse_type(parser);
            BUF_PUSH(node->fn_decl.params, param);
        } while (parser_match(parser, TOKEN_COMMA));
    }
    parser_expect(parser, TOKEN_RIGHT_PAREN);

    // Return type
    node->fn_decl.return_type.kind = AST_TYPE_INFERRED;
    if (parser_match(parser, TOKEN_ARROW)) {
        node->fn_decl.return_type = parser_parse_type(parser);
    }

    parser_skip_newlines(parser);

    // Body: block or `= expr`
    if (parser_check(parser, TOKEN_LEFT_BRACE)) {
        node->fn_decl.body = parser_parse_block(parser);
    } else if (parser_match(parser, TOKEN_EQUAL)) {
        node->fn_decl.body = parser_parse_expr(parser);
    } else {
        rsg_err(parser_current_loc(parser), "expected fn body");
    }

    return node;
}

// ── Top-level decl dispatch ─────────────────────────────────────

ASTNode *parser_parse_decl(Parser *parser) {
    parser_skip_newlines(parser);

    // module
    if (parser_check(parser, TOKEN_MODULE)) {
        SrcLoc loc = parser_current_loc(parser);
        parser_advance(parser); // consume 'module'
        ASTNode *node = ast_new(parser->arena, NODE_MODULE, loc);
        node->module.name = parser_expect(parser, TOKEN_ID)->lexeme;
        return node;
    }

    // struct
    if (parser_check(parser, TOKEN_STRUCT)) {
        return parse_struct_decl(parser);
    }

    // enum
    if (parser_check(parser, TOKEN_ENUM)) {
        return parse_enum_decl(parser);
    }

    // pact
    if (parser_check(parser, TOKEN_PACT)) {
        return parse_pact_decl(parser);
    }

    // type alias: type Name = UnderlyingType
    if (parser_check(parser, TOKEN_TYPE)) {
        SrcLoc loc = parser_current_loc(parser);
        parser_advance(parser); // consume 'type'
        ASTNode *node = ast_new(parser->arena, NODE_TYPE_ALIAS, loc);
        node->type_alias.name = parser_expect(parser, TOKEN_ID)->lexeme;
        parser_expect(parser, TOKEN_EQUAL);
        node->type_alias.alias_type = parser_parse_type(parser);
        return node;
    }

    // pub fn ...
    if (parser_check(parser, TOKEN_PUB)) {
        parser_advance(parser); // consume 'pub'
        parser_skip_newlines(parser);
        if (parser_check(parser, TOKEN_FN)) {
            return parse_fn_decl(parser, true);
        }
        rsg_err(parser_current_loc(parser), "expected 'fn' after 'pub'");
        return NULL;
    }

    // fn ...
    if (parser_check(parser, TOKEN_FN)) {
        return parse_fn_decl(parser, false);
    }

    // Top-level stmt (for scripts)
    return parser_parse_stmt(parser);
}
