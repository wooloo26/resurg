#include "_parse.h"

// ── Forward declarations ──────────────────────────────────────────────
static ASTTypeParam *parse_type_params(Parser *parser);
static ASTWhereClause *parse_where_clauses(Parser *parser);

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
    node->pact_decl.where_clauses = NULL;
    node->pact_decl.assoc_types = NULL;
    node->pact_decl.type_params = parse_type_params(parser);

    // Optional where clauses
    parser_skip_newlines(parser);
    node->pact_decl.where_clauses = parse_where_clauses(parser);

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
        } else if (parser_check(parser, TOKEN_TYPE)) {
            // Associated type: type Name, type Name: Bound, type Name = Default
            parser_advance(parser); // consume 'type'
            ASTAssocType at = {0};
            at.name = parser_expect(parser, TOKEN_ID)->lexeme;
            at.bounds = NULL;
            at.concrete_type = NULL;
            if (parser_match(parser, TOKEN_COLON)) {
                do {
                    const char *bound = parser_expect(parser, TOKEN_ID)->lexeme;
                    BUF_PUSH(at.bounds, bound);
                } while (parser_match(parser, TOKEN_PLUS));
            }
            if (parser_match(parser, TOKEN_EQUAL)) {
                at.concrete_type = arena_alloc_zero(parser->arena, sizeof(ASTType));
                *at.concrete_type = parser_parse_type(parser);
            }
            BUF_PUSH(node->pact_decl.assoc_types, at);
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
    node->enum_decl.where_clauses = NULL;
    node->enum_decl.type_params = parse_type_params(parser);

    // Optional where clauses
    parser_skip_newlines(parser);
    node->enum_decl.where_clauses = parse_where_clauses(parser);

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
    node->struct_decl.where_clauses = NULL;
    node->struct_decl.assoc_types = NULL;
    node->struct_decl.type_params = parse_type_params(parser);

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

    // Optional where clauses
    parser_skip_newlines(parser);
    node->struct_decl.where_clauses = parse_where_clauses(parser);

    parser_skip_newlines(parser);
    parser_expect(parser, TOKEN_LEFT_BRACE);
    parser_skip_newlines(parser);

    while (!parser_check(parser, TOKEN_RIGHT_BRACE) && !parser_at_end(parser)) {
        if (parser_check(parser, TOKEN_FN)) {
            // Method decl
            ASTNode *method = parse_method_decl(parser, node->struct_decl.name);
            BUF_PUSH(node->struct_decl.methods, method);
        } else if (parser_check(parser, TOKEN_TYPE)) {
            // Associated type: type Name = ConcreteType  OR  type Pact::Name = ConcreteType
            parser_advance(parser); // consume 'type'
            ASTAssocType at = {0};
            at.pact_qualifier = NULL;
            at.name = parser_expect(parser, TOKEN_ID)->lexeme;
            if (parser_match(parser, TOKEN_COLON_COLON)) {
                // Qualified: type Pact::Name = ...
                at.pact_qualifier = at.name;
                at.name = parser_expect(parser, TOKEN_ID)->lexeme;
            }
            at.bounds = NULL;
            at.concrete_type = NULL;
            if (parser_match(parser, TOKEN_EQUAL)) {
                at.concrete_type = arena_alloc_zero(parser->arena, sizeof(ASTType));
                *at.concrete_type = parser_parse_type(parser);
            }
            BUF_PUSH(node->struct_decl.assoc_types, at);
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
 * Parse generic type param list: `<T, U: Bound1 + Bound2, comptime N: usize, V = str>`.
 * Returns a stretchy buf of ASTTypeParam, or NULL if no `<` present.
 */
static ASTTypeParam *parse_type_params(Parser *parser) {
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
static ASTWhereClause *parse_where_clauses(Parser *parser) {
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
    node->fn_decl.where_clauses = NULL;
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

    // Optional where clauses
    parser_skip_newlines(parser);
    node->fn_decl.where_clauses = parse_where_clauses(parser);

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

// ── Ext decl ────────────────────────────────────────────────────

/**
 * Parse ext block:
 *   ext [<TypeParams>] TypeName [<TypeArgs>] [impl Pact [+ Pact2]] { methods }
 */
static ASTNode *parse_ext_decl(Parser *parser) {
    SrcLoc loc = parser_current_loc(parser);
    parser_expect(parser, TOKEN_EXT);

    ASTNode *node = ast_new(parser->arena, NODE_EXT_DECL, loc);
    node->ext_decl.type_params = NULL;
    node->ext_decl.target_name = NULL;
    node->ext_decl.target_type_args = NULL;
    node->ext_decl.impl_pacts = NULL;
    node->ext_decl.methods = NULL;

    // Optional type params: ext<T, U>
    node->ext_decl.type_params = parse_type_params(parser);

    // Target type name (struct, enum, or primitive type keyword)
    if (parser_check(parser, TOKEN_ID) ||
        token_is_type_keyword(parser_current_token(parser)->kind)) {
        node->ext_decl.target_name = parser_advance(parser)->lexeme;
    } else {
        rsg_err(parser_current_loc(parser), "expected type name after 'ext'");
        return node;
    }

    // Optional type args: ext Pair<i32, str> or ext<T, U> Pair<T, U>
    if (parser_match(parser, TOKEN_LESS)) {
        do {
            ASTType *ta = arena_alloc_zero(parser->arena, sizeof(ASTType));
            *ta = parser_parse_type(parser);
            BUF_PUSH(node->ext_decl.target_type_args, *ta);
        } while (parser_match(parser, TOKEN_COMMA));
        parser_expect(parser, TOKEN_GREATER);
    }

    // Optional pact impl: ext Type impl Pact1 + Pact2
    if (parser_match(parser, TOKEN_IMPL)) {
        do {
            const char *pact_name = parser_expect(parser, TOKEN_ID)->lexeme;
            BUF_PUSH(node->ext_decl.impl_pacts, pact_name);
        } while (parser_match(parser, TOKEN_PLUS));
    }

    parser_skip_newlines(parser);
    parser_expect(parser, TOKEN_LEFT_BRACE);
    parser_skip_newlines(parser);

    while (!parser_check(parser, TOKEN_RIGHT_BRACE) && !parser_at_end(parser)) {
        if (parser_check(parser, TOKEN_FN)) {
            ASTNode *method = parse_method_decl(parser, node->ext_decl.target_name);
            BUF_PUSH(node->ext_decl.methods, method);
        } else {
            rsg_err(parser_current_loc(parser), "expected method in ext block");
            parser_advance(parser);
        }
        parser_skip_newlines(parser);
    }

    parser_expect(parser, TOKEN_RIGHT_BRACE);
    return node;
}

// ── Use decl ────────────────────────────────────────────────────

/**
 * Parse use import:
 *   use module_name { name1, name2 as alias, ... }
 *   use module_name
 *   use super::name
 *   use super::super::name
 *   use self::name
 */
static ASTNode *parse_use_decl(Parser *parser) {
    SrcLoc loc = parser_current_loc(parser);
    parser_expect(parser, TOKEN_USE);

    ASTNode *node = ast_new(parser->arena, NODE_USE_DECL, loc);
    node->use_decl.module_path = parser_expect(parser, TOKEN_ID)->lexeme;
    node->use_decl.imported_names = NULL;
    node->use_decl.aliases = NULL;

    // Handle path-style use: use super::name or use super::super::name
    // Consume :: chain, building the module path; the last segment is the imported name
    if (parser_check(parser, TOKEN_COLON_COLON)) {
        // Collect segments: module_path :: seg1 :: seg2 :: ... :: name
        const char **segments = NULL;
        BUF_PUSH(segments, node->use_decl.module_path);
        while (parser_match(parser, TOKEN_COLON_COLON)) {
            const char *seg = parser_expect(parser, TOKEN_ID)->lexeme;
            BUF_PUSH(segments, seg);
        }
        // Last segment is the imported name, rest is the module path
        int32_t seg_count = BUF_LEN(segments);
        const char *imported = segments[seg_count - 1];
        BUF_PUSH(node->use_decl.imported_names, imported);
        BUF_PUSH(node->use_decl.aliases, imported);
        // Rebuild module path from segments[0..seg_count-2]
        const char *path = segments[0];
        for (int32_t i = 1; i < seg_count - 1; i++) {
            path = arena_sprintf(parser->arena, "%s::%s", path, segments[i]);
        }
        node->use_decl.module_path = path;
        BUF_FREE(segments);
        return node;
    }

    parser_skip_newlines(parser);

    // Selective import: use module { name1, name2 as alias }
    if (parser_match(parser, TOKEN_LEFT_BRACE)) {
        parser_skip_newlines(parser);
        while (!parser_check(parser, TOKEN_RIGHT_BRACE) && !parser_at_end(parser)) {
            const char *name = parser_expect(parser, TOKEN_ID)->lexeme;
            BUF_PUSH(node->use_decl.imported_names, name);

            // Check for alias: name as alias
            if (parser_check(parser, TOKEN_ID) &&
                strcmp(parser_current_token(parser)->lexeme, "as") == 0) {
                parser_advance(parser); // consume 'as'
                const char *alias = parser_expect(parser, TOKEN_ID)->lexeme;
                BUF_PUSH(node->use_decl.aliases, alias);
            } else {
                BUF_PUSH(node->use_decl.aliases, name);
            }

            parser_match(parser, TOKEN_COMMA);
            parser_skip_newlines(parser);
        }
        parser_expect(parser, TOKEN_RIGHT_BRACE);
    }

    return node;
}

// ── Nested module decl ──────────────────────────────────────────

/**
 * Parse nested module: module name { decls }
 * or flat module declaration: module name
 */
static ASTNode *parse_module_decl(Parser *parser, bool is_pub) {
    SrcLoc loc = parser_current_loc(parser);
    parser_advance(parser); // consume 'mod'
    ASTNode *node = ast_new(parser->arena, NODE_MODULE, loc);
    node->module.name = parser_expect(parser, TOKEN_ID)->lexeme;
    node->module.is_pub = is_pub;
    node->module.decls = NULL;

    parser_skip_newlines(parser);

    // Nested module body: module name { decls }
    if (parser_match(parser, TOKEN_LEFT_BRACE)) {
        parser_skip_newlines(parser);
        while (!parser_check(parser, TOKEN_RIGHT_BRACE) && !parser_at_end(parser)) {
            ASTNode *decl = parser_parse_decl(parser);
            if (decl != NULL) {
                BUF_PUSH(node->module.decls, decl);
            }
            parser_skip_newlines(parser);
        }
        parser_expect(parser, TOKEN_RIGHT_BRACE);
    }

    return node;
}

// ── Top-level decl dispatch ─────────────────────────────────────

ASTNode *parser_parse_decl(Parser *parser) {
    parser_skip_newlines(parser);

    // module
    if (parser_check(parser, TOKEN_MODULE)) {
        return parse_module_decl(parser, false);
    }

    // ext
    if (parser_check(parser, TOKEN_EXT)) {
        return parse_ext_decl(parser);
    }

    // use
    if (parser_check(parser, TOKEN_USE)) {
        return parse_use_decl(parser);
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
        node->type_alias.is_pub = false;
        node->type_alias.type_params = parse_type_params(parser);
        parser_expect(parser, TOKEN_EQUAL);
        node->type_alias.alias_type = parser_parse_type(parser);
        return node;
    }

    // pub ...
    if (parser_check(parser, TOKEN_PUB)) {
        parser_advance(parser); // consume 'pub'
        parser_skip_newlines(parser);
        if (parser_check(parser, TOKEN_FN)) {
            return parse_fn_decl(parser, true);
        }
        if (parser_check(parser, TOKEN_STRUCT)) {
            ASTNode *node = parse_struct_decl(parser);
            node->struct_decl.is_pub = true;
            return node;
        }
        if (parser_check(parser, TOKEN_ENUM)) {
            ASTNode *node = parse_enum_decl(parser);
            node->enum_decl.is_pub = true;
            return node;
        }
        if (parser_check(parser, TOKEN_PACT)) {
            ASTNode *node = parse_pact_decl(parser);
            node->pact_decl.is_pub = true;
            return node;
        }
        if (parser_check(parser, TOKEN_TYPE)) {
            SrcLoc loc = parser_current_loc(parser);
            parser_advance(parser); // consume 'type'
            ASTNode *node = ast_new(parser->arena, NODE_TYPE_ALIAS, loc);
            node->type_alias.name = parser_expect(parser, TOKEN_ID)->lexeme;
            node->type_alias.is_pub = true;
            node->type_alias.type_params = parse_type_params(parser);
            parser_expect(parser, TOKEN_EQUAL);
            node->type_alias.alias_type = parser_parse_type(parser);
            return node;
        }
        if (parser_check(parser, TOKEN_MODULE)) {
            return parse_module_decl(parser, true);
        }
        rsg_err(parser_current_loc(parser),
                "expected 'fn', 'struct', 'enum', 'pact', 'type', or 'mod' after 'pub'");
        return NULL;
    }

    // fn ...
    if (parser_check(parser, TOKEN_FN)) {
        return parse_fn_decl(parser, false);
    }

    // Top-level stmt (for scripts)
    return parser_parse_stmt(parser);
}
