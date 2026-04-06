#include "_parser.h"

// ── Token-stream navigation helpers ────────────────────────────────────

const Token *parser_current_token(const Parser *parser) {
    return &parser->tokens[parser->position];
}

const Token *parser_previous_token(const Parser *parser) {
    return &parser->tokens[parser->position - 1];
}

bool parser_at_end(const Parser *parser) {
    return parser_current_token(parser)->kind == TOKEN_EOF;
}

bool parser_check(const Parser *parser, TokenKind kind) {
    return parser_current_token(parser)->kind == kind;
}

const Token *parser_advance(Parser *parser) {
    if (!parser_at_end(parser)) {
        parser->position++;
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
    rsg_error(parser_current_token(parser)->location, "expected '%s', got '%s'",
              token_kind_string(kind), token_kind_string(parser_current_token(parser)->kind));
    return parser_current_token(parser);
}

void parser_skip_newlines(Parser *parser) {
    while (parser_check(parser, TOKEN_NEWLINE)) {
        parser_advance(parser);
    }
}

SourceLocation parser_current_location(const Parser *parser) {
    return parser_current_token(parser)->location;
}

// ── Parser lifecycle ───────────────────────────────────────────────────

Parser *parser_create(const Token *tokens, int32_t count, Arena *arena, const char *file) {
    Parser *parser = rsg_malloc(sizeof(*parser));
    parser->tokens = tokens;
    parser->position = 0;
    parser->count = count;
    parser->error_count = 0;
    parser->arena = arena;
    parser->file = file;
    return parser;
}

void parser_destroy(Parser *parser) {
    free(parser);
}

int32_t parser_error_count(const Parser *parser) {
    return parser->error_count;
}

ASTNode *parser_parse(Parser *parser) {
    SourceLocation location = {.file = parser->file, .line = 1, .column = 1};
    ASTNode *file = ast_new(parser->arena, NODE_FILE, location);
    file->file.declarations = NULL;

    parser_skip_newlines(parser);
    while (!parser_at_end(parser)) {
        ASTNode *declaration = parser_parse_declaration(parser);
        if (declaration != NULL) {
            BUFFER_PUSH(file->file.declarations, declaration);
        }
        parser_skip_newlines(parser);
    }

    return file;
}
