#include "_parse.h"

// ── Token-stream navigation helpers ────────────────────────────────────

const Token *parser_current_token(const Parser *parser) {
    return &parser->tokens[parser->pos];
}

const Token *parser_previous_token(const Parser *parser) {
    return &parser->tokens[parser->pos - 1];
}

bool parser_at_end(const Parser *parser) {
    return parser_current_token(parser)->kind == TOKEN_EOF;
}

bool parser_check(const Parser *parser, TokenKind kind) {
    return parser_current_token(parser)->kind == kind;
}

const Token *parser_advance(Parser *parser) {
    if (!parser_at_end(parser)) {
        parser->pos++;
    }
    return parser_previous_token(parser);
}

bool parser_match(Parser *parser, TokenKind kind) {
    if (parser_check(parser, kind)) {
        parser_advance(parser);
        return true;
    }
    return false;
}

const Token *parser_expect(Parser *parser, TokenKind kind) {
    if (parser_check(parser, kind)) {
        return parser_advance(parser);
    }
    PARSER_ERR(parser, parser_current_token(parser)->loc, "expected '%s', got '%s'",
               token_kind_str(kind), token_kind_str(parser_current_token(parser)->kind));
    return parser_current_token(parser);
}

void parser_skip_newlines(Parser *parser) {
    while (parser_check(parser, TOKEN_NEWLINE) || parser_check(parser, TOKEN_SEMICOLON)) {
        parser_advance(parser);
    }
}

SrcLoc parser_current_loc(const Parser *parser) {
    return parser_current_token(parser)->loc;
}

// ── Pattern-binding lookahead ──────────────────────────────────────────

bool parser_is_pattern_binding(const Parser *parser) {
    if (!parser_check(parser, TOKEN_ID)) {
        return false;
    }
    int32_t pos = parser->pos + 1;
    if (pos >= parser->count) {
        return false;
    }

    // ID :=  (simple binding pattern)
    if (parser->tokens[pos].kind == TOKEN_COLON_EQUAL) {
        return true;
    }

    // ID( ... ) :=  (tuple variant pattern, e.g. Some(x) := expr)
    if (parser->tokens[pos].kind == TOKEN_LEFT_PAREN) {
        pos++; // skip '('
        int depth = 1;
        while (pos < parser->count && depth > 0) {
            if (parser->tokens[pos].kind == TOKEN_LEFT_PAREN) {
                depth++;
            } else if (parser->tokens[pos].kind == TOKEN_RIGHT_PAREN) {
                depth--;
            }
            pos++;
        }
        // pos is now past the closing ')'
        return pos < parser->count && parser->tokens[pos].kind == TOKEN_COLON_EQUAL;
    }

    return false;
}

// ── Parser lifecycle ───────────────────────────────────────────────────

Parser *parser_create(const Token *tokens, int32_t count, Arena *arena, const char *file) {
    Parser *parser = rsg_malloc(sizeof(*parser));
    parser->tokens = tokens;
    parser->pos = 0;
    parser->count = count;
    parser->err_count = 0;
    parser->arena = arena;
    parser->file = file;
    parser->no_struct_lit = false;
    return parser;
}

void parser_destroy(Parser *parser) {
    free(parser);
}

int32_t parser_err_count(const Parser *parser) {
    return parser->err_count;
}

ASTNode *parser_parse(Parser *parser) {
    SrcLoc loc = {.file = parser->file, .line = 1, .column = 1};
    ASTNode *file = ast_new(parser->arena, NODE_FILE, loc);
    file->file.decls = NULL;

    parser_skip_newlines(parser);
    while (!parser_at_end(parser)) {
        ASTNode *decl = parser_parse_decl(parser);
        if (decl != NULL) {
            BUF_PUSH(file->file.decls, decl);
        }
        parser_skip_newlines(parser);
    }

    return file;
}
