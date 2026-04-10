/**
 * @file parse_decl.c
 * @brief Declaration parsing — struct, enum, pact, ext, use, module, and top-level dispatch.
 */

#include "_parse.h"

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

            if (parser_check(parser, TOKEN_COLON)) {
                // Reject field declarations: pacts define methods only
                PARSER_ERR(parser, parser_current_loc(parser),
                           "expected method or pact name in pact declaration; "
                           "fields are not supported in pacts");
                parser_advance(parser); // consume ':'
                parser_parse_type(parser);
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
            PARSER_ERR(parser, parser_current_loc(parser),
                       "expected method or pact name in pact declaration");
            parser_advance(parser);
        }
        parser_skip_newlines(parser);
    }

    parser_expect(parser, TOKEN_RIGHT_BRACE);
    return node;
}

// ── Enum variant sub-parsers ───────────────────────────────────────

/** Parse a tuple variant body: (type1, type2, ...) */
static void parse_variant_tuple_body(Parser *parser, ASTEnumVariant *variant) {
    variant->kind = VARIANT_TUPLE;
    if (!parser_check(parser, TOKEN_RIGHT_PAREN)) {
        do {
            ASTType *elem = arena_alloc_zero(parser->arena, sizeof(ASTType));
            *elem = parser_parse_type(parser);
            BUF_PUSH(variant->tuple_types, *elem);
        } while (parser_match(parser, TOKEN_COMMA));
    }
    parser_expect(parser, TOKEN_RIGHT_PAREN);
}

/** Parse a struct variant body: { field: type, ... } */
static void parse_variant_struct_body(Parser *parser, ASTEnumVariant *variant) {
    variant->kind = VARIANT_STRUCT;
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
            BUF_PUSH(variant->fields, field);
        } while (parser_match(parser, TOKEN_COMMA));
    }
    parser_skip_newlines(parser);
    parser_expect(parser, TOKEN_RIGHT_BRACE);
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
    node->enum_decl.assoc_types = NULL;
    node->enum_decl.type_params = parse_type_params(parser);

    // Optional where clauses
    parser_skip_newlines(parser);
    node->enum_decl.where_clauses = parse_where_clauses(parser);

    parser_skip_newlines(parser);
    parser_expect(parser, TOKEN_LEFT_BRACE);
    parser_skip_newlines(parser);

    while (!parser_check(parser, TOKEN_RIGHT_BRACE) && !parser_at_end(parser)) {
        if (parser_check(parser, TOKEN_FN)) {
            // Reject inline methods: use ext blocks instead
            PARSER_ERR(parser, parser_current_loc(parser),
                       "expected enum variant in enum block; "
                       "methods must be defined in ext blocks");
            parse_method_decl(parser, node->enum_decl.name, false);
        } else if (parser_check(parser, TOKEN_TYPE)) {
            // Reject inline associated types: use ext impl blocks instead
            PARSER_ERR(parser, parser_current_loc(parser),
                       "expected enum variant in enum block; "
                       "associated types must be defined in ext impl blocks");
            parser_advance(parser); // consume 'type'
            parser_expect(parser, TOKEN_ID);
            if (parser_match(parser, TOKEN_COLON)) {
                do {
                    parser_expect(parser, TOKEN_ID);
                } while (parser_match(parser, TOKEN_PLUS));
            }
            if (parser_match(parser, TOKEN_EQUAL)) {
                parser_parse_type(parser);
            }
        } else if (parser_check(parser, TOKEN_ID)) {
            ASTEnumVariant variant = {0};
            variant.name = parser_advance(parser)->lexeme;
            variant.kind = VARIANT_UNIT;
            variant.tuple_types = NULL;
            variant.fields = NULL;
            variant.discriminant = NULL;

            if (parser_match(parser, TOKEN_LEFT_PAREN)) {
                parse_variant_tuple_body(parser, &variant);
            } else if (parser_check(parser, TOKEN_LEFT_BRACE)) {
                parse_variant_struct_body(parser, &variant);
            } else if (parser_match(parser, TOKEN_EQUAL)) {
                // Explicit discriminant: Name = expr
                variant.discriminant = parser_parse_expr(parser);
            }

            BUF_PUSH(node->enum_decl.variants, variant);
        } else {
            PARSER_ERR(parser, parser_current_loc(parser), "expected enum variant");
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
    node->struct_decl.is_tuple_struct = false;
    node->struct_decl.type_params = parse_type_params(parser);

    // Reject conformance lists: use ext blocks instead
    if (parser_check(parser, TOKEN_COLON)) {
        PARSER_ERR(parser, parser_current_loc(parser),
                   "conformance lists on struct declarations are not supported; "
                   "use 'ext Type impl Pact { ... }' instead");
        parser_advance(parser); // consume ':'
        // Consume the conformance list to recover
        do {
            parser_skip_newlines(parser);
            if (parser_check(parser, TOKEN_ID)) {
                parser_advance(parser);
            }
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

    // Tuple struct: struct Name(Type, ...)
    if (parser_match(parser, TOKEN_LEFT_PAREN)) {
        node->struct_decl.is_tuple_struct = true;
        int32_t idx = 0;
        do {
            parser_skip_newlines(parser);
            if (parser_check(parser, TOKEN_RIGHT_PAREN)) {
                break;
            }
            ASTStructField field = {0};
            field.name = arena_sprintf(parser->arena, "_%d", idx);
            field.type = parser_parse_type(parser);
            field.default_value = NULL;
            BUF_PUSH(node->struct_decl.fields, field);
            idx++;
        } while (parser_match(parser, TOKEN_COMMA));
        parser_expect(parser, TOKEN_RIGHT_PAREN);
        return node;
    }

    parser_expect(parser, TOKEN_LEFT_BRACE);
    parser_skip_newlines(parser);

    while (!parser_check(parser, TOKEN_RIGHT_BRACE) && !parser_at_end(parser)) {
        if (parser_check(parser, TOKEN_FN)) {
            // Reject inline methods: use ext blocks instead
            PARSER_ERR(parser, parser_current_loc(parser),
                       "expected field or embedded struct in struct block; "
                       "methods must be defined in ext blocks");
            parse_method_decl(parser, node->struct_decl.name, false);
        } else if (parser_check(parser, TOKEN_TYPE)) {
            // Reject inline associated types: use ext impl blocks instead
            PARSER_ERR(parser, parser_current_loc(parser),
                       "expected field or embedded struct in struct block; "
                       "associated types must be defined in ext impl blocks");
            parser_advance(parser); // consume 'type'
            parser_expect(parser, TOKEN_ID);
            if (parser_match(parser, TOKEN_COLON_COLON)) {
                parser_expect(parser, TOKEN_ID);
            }
            if (parser_match(parser, TOKEN_EQUAL)) {
                parser_parse_type(parser);
            }
        } else if (parser_check(parser, TOKEN_PUB) || parser_check(parser, TOKEN_ID)) {
            bool is_pub_field = parser_match(parser, TOKEN_PUB);
            const char *name = parser_expect(parser, TOKEN_ID)->lexeme;

            if (parser_match(parser, TOKEN_COLON)) {
                // Field: [pub] name: Type [= default]
                ASTStructField field = {0};
                field.name = name;
                field.is_pub = is_pub_field;
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
            PARSER_ERR(parser, parser_current_loc(parser), "expected field or embedded struct");
            parser_advance(parser);
        }
        parser_skip_newlines(parser);
    }

    parser_expect(parser, TOKEN_RIGHT_BRACE);
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
    node->ext_decl.assoc_types = NULL;

    // Optional type params: ext<T, U>
    node->ext_decl.type_params = parse_type_params(parser);

    // Target type name (struct, enum, primitive, or compound type)
    if (parser_check(parser, TOKEN_LEFT_BRACKET)) {
        // Compound type: ext<T> []T (slice) or ext<T, comptime N: usize> [N]T (array)
        if (BUF_LEN(node->ext_decl.type_params) == 0) {
            PARSER_ERR(parser, parser_current_loc(parser),
                       "compound ext blocks require type parameters: "
                       "use 'ext<T> []T' or 'ext<T, comptime N: usize> [N]T'");
        }
        parser_advance(parser); // consume '['
        if (parser_check(parser, TOKEN_RIGHT_BRACKET)) {
            parser_advance(parser); // consume ']'
            node->ext_decl.target_name = "[]";
        } else {
            parser_advance(parser); // consume size placeholder (e.g., '_' or N)
            parser_expect(parser, TOKEN_RIGHT_BRACKET);
            node->ext_decl.target_name = "[_]";
        }
        // Consume element type name (e.g., T in ext<T> []T)
        if (parser_check(parser, TOKEN_ID) ||
            token_is_type_keyword(parser_current_token(parser)->kind)) {
            parser_advance(parser);
        }
    } else if (parser_check(parser, TOKEN_ID) ||
               token_is_type_keyword(parser_current_token(parser)->kind)) {
        node->ext_decl.target_name = parser_advance(parser)->lexeme;
    } else {
        PARSER_ERR(parser, parser_current_loc(parser), "expected type name after 'ext'");
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
        bool method_is_pub = parser_match(parser, TOKEN_PUB);
        if (parser_check(parser, TOKEN_FN) || parser_check(parser, TOKEN_DECLARE)) {
            ASTNode *method = parse_method_decl(parser, node->ext_decl.target_name, method_is_pub);
            BUF_PUSH(node->ext_decl.methods, method);
        } else if (parser_check(parser, TOKEN_TYPE)) {
            // Associated type: type Name = ConcreteType  OR  type Pact::Name = ConcreteType
            parser_advance(parser); // consume 'type'
            ASTAssocType at = {0};
            at.pact_qualifier = NULL;
            at.name = parser_expect(parser, TOKEN_ID)->lexeme;
            if (parser_match(parser, TOKEN_COLON_COLON)) {
                at.pact_qualifier = at.name;
                at.name = parser_expect(parser, TOKEN_ID)->lexeme;
            }
            at.bounds = NULL;
            at.concrete_type = NULL;
            if (parser_match(parser, TOKEN_EQUAL)) {
                at.concrete_type = arena_alloc_zero(parser->arena, sizeof(ASTType));
                *at.concrete_type = parser_parse_type(parser);
            }
            BUF_PUSH(node->ext_decl.assoc_types, at);
        } else {
            PARSER_ERR(parser, parser_current_loc(parser), "expected method in ext block");
            parser_advance(parser);
        }
        parser_skip_newlines(parser);
    }

    parser_expect(parser, TOKEN_RIGHT_BRACE);
    return node;
}

// ── Use decl ────────────────────────────────────────────────────

/**
 * Parse use import (v0.9.7 path-style syntax):
 *   use module::{name1, name2 as alias, ...}
 *   use module::name [as alias]
 *   use super::name
 *   use super::super::name
 *   use outer::inner::{name1, name2}
 *   use outer::inner::name [as alias]
 */
static ASTNode *parse_use_decl(Parser *parser, bool is_pub) {
    SrcLoc loc = parser_current_loc(parser);
    parser_expect(parser, TOKEN_USE);

    ASTNode *node = ast_new(parser->arena, NODE_USE_DECL, loc);
    node->use_decl.imported_names = NULL;
    node->use_decl.aliases = NULL;
    node->use_decl.is_pub = is_pub;
    node->use_decl.is_wildcard = false;

    // Collect path segments separated by ::
    const char **segments = NULL;
    BUF_PUSH(segments, parser_expect(parser, TOKEN_ID)->lexeme);

    bool selective = false;
    bool wildcard = false;
    while (parser_match(parser, TOKEN_COLON_COLON)) {
        if (parser_check(parser, TOKEN_LEFT_BRACE)) {
            selective = true;
            break;
        }
        if (parser_check(parser, TOKEN_STAR)) {
            parser_advance(parser); // consume '*'
            wildcard = true;
            break;
        }
        BUF_PUSH(segments, parser_expect(parser, TOKEN_ID)->lexeme);
    }

    // Require at least one :: (e.g., use module::name or use module::{...})
    if (BUF_LEN(segments) < 2 && !selective && !wildcard) {
        PARSER_ERR(parser, loc, "expected '::' after module name in use declaration");
    }

    if (wildcard) {
        // Wildcard import: use path::*
        const char *path = segments[0];
        for (int32_t i = 1; i < BUF_LEN(segments); i++) {
            path = arena_sprintf(parser->arena, "%s::%s", path, segments[i]);
        }
        node->use_decl.module_path = path;
        node->use_decl.is_wildcard = true;
    } else if (selective) {
        // Selective import: use path::{name1, name2 as alias, ...}
        // Build module path from all segments
        const char *path = segments[0];
        for (int32_t i = 1; i < BUF_LEN(segments); i++) {
            path = arena_sprintf(parser->arena, "%s::%s", path, segments[i]);
        }
        node->use_decl.module_path = path;

        parser_advance(parser); // consume '{'
        parser_skip_newlines(parser);
        while (!parser_check(parser, TOKEN_RIGHT_BRACE) && !parser_at_end(parser)) {
            const char *name = parser_expect(parser, TOKEN_ID)->lexeme;
            BUF_PUSH(node->use_decl.imported_names, name);

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
    } else {
        // Single path import: use module::name [as alias]
        int32_t seg_count = BUF_LEN(segments);
        const char *imported = segments[seg_count - 1];

        const char *alias = imported;
        if (parser_check(parser, TOKEN_ID) &&
            strcmp(parser_current_token(parser)->lexeme, "as") == 0) {
            parser_advance(parser); // consume 'as'
            alias = parser_expect(parser, TOKEN_ID)->lexeme;
        }

        BUF_PUSH(node->use_decl.imported_names, imported);
        BUF_PUSH(node->use_decl.aliases, alias);

        // Build module path from segments[0..seg_count-2]
        const char *path = segments[0];
        for (int32_t i = 1; i < seg_count - 1; i++) {
            path = arena_sprintf(parser->arena, "%s::%s", path, segments[i]);
        }
        node->use_decl.module_path = path;
    }

    BUF_FREE(segments);
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

    // declare fn / declare var
    if (parser_check(parser, TOKEN_DECLARE)) {
        if (parser_peek_is(parser, TOKEN_FN)) {
            return parse_fn_decl(parser, false);
        }
        if (parser_peek_is(parser, TOKEN_VAR)) {
            parser_advance(parser); // consume 'declare'
            return parse_declare_var(parser, false);
        }
        PARSER_ERR(parser, parser_current_loc(parser), "expected 'fn' or 'var' after 'declare'");
        parser_advance(parser);
        return NULL;
    }

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
        return parse_use_decl(parser, false);
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
        if (parser_check(parser, TOKEN_DECLARE)) {
            if (parser_peek_is(parser, TOKEN_FN)) {
                return parse_fn_decl(parser, true);
            }
            if (parser_peek_is(parser, TOKEN_VAR)) {
                parser_advance(parser); // consume 'declare'
                return parse_declare_var(parser, true);
            }
            PARSER_ERR(parser, parser_current_loc(parser),
                       "expected 'fn' or 'var' after 'declare'");
            parser_advance(parser);
            return NULL;
        }
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
        if (parser_check(parser, TOKEN_USE)) {
            return parse_use_decl(parser, true);
        }
        PARSER_ERR(parser, parser_current_loc(parser),
                   "expected 'fn', 'struct', 'enum', 'pact', 'type', 'mod', or 'use' after 'pub'");
        return NULL;
    }

    // fn ...
    if (parser_check(parser, TOKEN_FN)) {
        return parse_fn_decl(parser, false);
    }

    // Top-level stmt (for scripts)
    return parser_parse_stmt(parser);
}
