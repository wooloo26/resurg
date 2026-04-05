#include "_lexer.h"

// Character classification helpers.
static bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool is_alnum(char c) {
    return is_alpha(c) || is_digit(c);
}

Token make_token(const Lexer *lexer, TokenKind kind, const char *start, int32_t length,
                 SourceLocation location) {
    return (Token){
        .kind = kind,
        .lexeme = arena_strndup(lexer->arena, start, length),
        .length = length,
        .location = location,
    };
}

/** Keyword lookup table - maps reserved words to their TokenKind. */
typedef struct {
    const char *word;
    TokenKind kind;
} Keyword;

static const Keyword KEYWORDS[] = {
    {"module", TOKEN_MODULE},     {"pub", TOKEN_PUBLIC},    {"fn", TOKEN_FUNCTION},
    {"var", TOKEN_VARIABLE},      {"if", TOKEN_IF},         {"else", TOKEN_ELSE},
    {"loop", TOKEN_LOOP},         {"for", TOKEN_FOR},       {"break", TOKEN_BREAK},
    {"continue", TOKEN_CONTINUE}, {"true", TOKEN_TRUE},     {"false", TOKEN_FALSE},
    {"type", TOKEN_TYPE},         {"struct", TOKEN_STRUCT}, {"mut", TOKEN_MUT},
    {"immut", TOKEN_IMMUT},       {"enum", TOKEN_ENUM},     {"match", TOKEN_MATCH},
    {"return", TOKEN_RETURN},     {"bool", TOKEN_BOOL},     {"i8", TOKEN_I8},
    {"i16", TOKEN_I16},           {"i32", TOKEN_I32},       {"i64", TOKEN_I64},
    {"i128", TOKEN_I128},         {"u8", TOKEN_U8},         {"u16", TOKEN_U16},
    {"u32", TOKEN_U32},           {"u64", TOKEN_U64},       {"u128", TOKEN_U128},
    {"isize", TOKEN_ISIZE},       {"usize", TOKEN_USIZE},   {"f32", TOKEN_F32},
    {"f64", TOKEN_F64},           {"char", TOKEN_CHAR},     {"str", TOKEN_STRING},
    {"unit", TOKEN_UNIT},
};

static const int32_t KEYWORD_COUNT = (int32_t)(sizeof(KEYWORDS) / sizeof(KEYWORDS[0]));

/**
 * Look up @p text (length @p len) in the keyword table.  Returns
 * TOKEN_IDENTIFIER if no keyword matches.
 */
static TokenKind lookup_keyword(const char *text, int32_t len) {
    for (int32_t i = 0; i < KEYWORD_COUNT; i++) {
        if ((int32_t)strlen(KEYWORDS[i].word) == len && memcmp(KEYWORDS[i].word, text, len) == 0) {
            return KEYWORDS[i].kind;
        }
    }
    return TOKEN_IDENTIFIER;
}

// Scanning routines - one per lexeme category.

/**
 * Scan a decimal integer or floating-point literal.  Underscores are
 * silently stripped before parsing the numeric value.
 */
static Token scan_number(Lexer *lexer, SourceLocation location) {
    const char *start = lexer->source + lexer->position - 1;
    bool is_float = false;

    while (is_digit(peek(lexer)) || peek(lexer) == '_') {
        advance(lexer);
    }

    if (peek(lexer) == '.' && peek_next(lexer) != '.' && lexer->last_kind != TOKEN_DOT) {
        is_float = true;
        advance(lexer); // consume '.'
        while (is_digit(peek(lexer)) || peek(lexer) == '_') {
            advance(lexer);
        }
    }

    if (peek(lexer) == 'e' || peek(lexer) == 'E') {
        is_float = true;
        advance(lexer); // consume 'e' or 'E'
        if (peek(lexer) == '+' || peek(lexer) == '-') {
            advance(lexer); // consume sign
        }
        while (is_digit(peek(lexer))) {
            advance(lexer);
        }
    }

    int32_t length = (int32_t)(lexer->source + lexer->position - start);
    TokenKind number_kind = is_float ? TOKEN_FLOAT_LITERAL : TOKEN_INTEGER_LITERAL;
    Token token = make_token(lexer, number_kind, start, length, location);

    // Strip underscores and parse value
    char buffer[64];
    int32_t buffer_index = 0;
    for (int32_t i = 0; i < length && buffer_index < 63; i++) {
        if (start[i] != '_') {
            buffer[buffer_index++] = start[i];
        }
    }
    buffer[buffer_index] = '\0';

    if (is_float) {
        token.literal_value.float_value = strtod(buffer, NULL);
    } else {
        token.literal_value.integer_value = strtoull(buffer, NULL, 10);
    }

    return token;
}

// scan_char_literal and scan_string are in scan_string.c

/** Reserved keywords that cannot be used as identifiers. */
static const char *const RESERVED_KEYWORDS[] = {
    "async", "await",  "comptime", "defer", "enum",  "macro", "match",
    "pact",  "return", "spawn",    "use",   "where", "while",
};

static const int32_t RESERVED_KEYWORD_COUNT =
    (int32_t)(sizeof(RESERVED_KEYWORDS) / sizeof(RESERVED_KEYWORDS[0]));

/** Return true if @p text (length @p len) is a reserved keyword. */
static bool is_reserved_keyword(const char *text, int32_t len) {
    for (int32_t i = 0; i < RESERVED_KEYWORD_COUNT; i++) {
        if ((int32_t)strlen(RESERVED_KEYWORDS[i]) == len &&
            memcmp(RESERVED_KEYWORDS[i], text, len) == 0) {
            return true;
        }
    }
    return false;
}

static Token scan_ident(Lexer *lexer, SourceLocation location) {
    const char *start = lexer->source + lexer->position - 1;
    while (is_alnum(peek(lexer))) {
        advance(lexer);
    }

    int32_t length = (int32_t)(lexer->source + lexer->position - start);
    TokenKind kind = lookup_keyword(start, length);

    if (kind == TOKEN_IDENTIFIER && is_reserved_keyword(start, length)) {
        rsg_error(location, "'%.*s' is a reserved keyword", length, start);
        return make_token(lexer, TOKEN_ERROR, start, length, location);
    }

    return make_token(lexer, kind, start, length, location);
}

/** Dispatch a comparison or logical operator token. */
static Token scan_comparison_or_logical(Lexer *lexer, char c, SourceLocation location) {
    switch (c) {
    case '=':
        if (match(lexer, '>')) {
            return make_token(lexer, TOKEN_FAT_ARROW, "=>", 2, location);
        }
        return match(lexer, '=') ? make_token(lexer, TOKEN_EQUAL_EQUAL, "==", 2, location)
                                 : make_token(lexer, TOKEN_EQUAL, "=", 1, location);
    case '!':
        return match(lexer, '=') ? make_token(lexer, TOKEN_BANG_EQUAL, "!=", 2, location)
                                 : make_token(lexer, TOKEN_BANG, "!", 1, location);
    case '<':
        return match(lexer, '=') ? make_token(lexer, TOKEN_LESS_EQUAL, "<=", 2, location)
                                 : make_token(lexer, TOKEN_LESS, "<", 1, location);
    case '>':
        return match(lexer, '=') ? make_token(lexer, TOKEN_GREATER_EQUAL, ">=", 2, location)
                                 : make_token(lexer, TOKEN_GREATER, ">", 1, location);
    case '&':
        if (match(lexer, '&')) {
            return make_token(lexer, TOKEN_AMPERSAND_AMPERSAND, "&&", 2, location);
        }
        return make_token(lexer, TOKEN_AMPERSAND, "&", 1, location);
    case '|':
        if (match(lexer, '|')) {
            return make_token(lexer, TOKEN_PIPE_PIPE, "||", 2, location);
        }
        return make_token(lexer, TOKEN_PIPE, "|", 1, location);
    default:
        break;
    }
    return make_token(lexer, TOKEN_ERROR, &lexer->source[lexer->position - 1], 1, location);
}

/** Dispatch an arithmetic operator token (with optional compound assignment). */
static Token scan_arithmetic(Lexer *lexer, char c, SourceLocation location) {
    switch (c) {
    case '+':
        return match(lexer, '=') ? make_token(lexer, TOKEN_PLUS_EQUAL, "+=", 2, location)
                                 : make_token(lexer, TOKEN_PLUS, "+", 1, location);
    case '-':
        if (match(lexer, '>')) {
            return make_token(lexer, TOKEN_ARROW, "->", 2, location);
        }
        return match(lexer, '=') ? make_token(lexer, TOKEN_MINUS_EQUAL, "-=", 2, location)
                                 : make_token(lexer, TOKEN_MINUS, "-", 1, location);
    case '*':
        return match(lexer, '=') ? make_token(lexer, TOKEN_STAR_EQUAL, "*=", 2, location)
                                 : make_token(lexer, TOKEN_STAR, "*", 1, location);
    case '/':
        return match(lexer, '=') ? make_token(lexer, TOKEN_SLASH_EQUAL, "/=", 2, location)
                                 : make_token(lexer, TOKEN_SLASH, "/", 1, location);
    case '%':
        return make_token(lexer, TOKEN_PERCENT, "%", 1, location);
    default:
        break;
    }
    return make_token(lexer, TOKEN_ERROR, &lexer->source[lexer->position - 1], 1, location);
}

/** Dispatch a single- or double-character punctuation / operator token. */
static Token scan_punctuation(Lexer *lexer, char c, SourceLocation location) {
    switch (c) {
    case ':':
        return match(lexer, '=') ? make_token(lexer, TOKEN_COLON_EQUAL, ":=", 2, location)
                                 : make_token(lexer, TOKEN_COLON, ":", 1, location);
    case '.':
        if (match(lexer, '.')) {
            return match(lexer, '=') ? make_token(lexer, TOKEN_DOT_DOT_EQUAL, "..=", 3, location)
                                     : make_token(lexer, TOKEN_DOT_DOT, "..", 2, location);
        }
        return make_token(lexer, TOKEN_DOT, ".", 1, location);
    case '(':
        return make_token(lexer, TOKEN_LEFT_PAREN, "(", 1, location);
    case ')':
        return make_token(lexer, TOKEN_RIGHT_PAREN, ")", 1, location);
    case '{':
        return make_token(lexer, TOKEN_LEFT_BRACE, "{", 1, location);
    case '}':
        return make_token(lexer, TOKEN_RIGHT_BRACE, "}", 1, location);
    case '[':
        return make_token(lexer, TOKEN_LEFT_BRACKET, "[", 1, location);
    case ']':
        return make_token(lexer, TOKEN_RIGHT_BRACKET, "]", 1, location);
    case ',':
        return make_token(lexer, TOKEN_COMMA, ",", 1, location);
    case ';':
        return make_token(lexer, TOKEN_SEMICOLON, ";", 1, location);
    case '=':
    case '!':
    case '<':
    case '>':
    case '&':
    case '|':
        return scan_comparison_or_logical(lexer, c, location);
    case '+':
    case '-':
    case '*':
    case '/':
    case '%':
        return scan_arithmetic(lexer, c, location);
    default:
        break;
    }
    rsg_error(location, "unexpected character '%c'", c);
    return make_token(lexer, TOKEN_ERROR, &lexer->source[lexer->position - 1], 1, location);
}

/**
 * Core scanner - skips whitespace and comments, then dispatches to the
 * appropriate scan_* routine.  Used by both lexer_next and interpolation
 * scanning.
 */
Token scan_token(Lexer *lexer) {
    // Skip horizontal whitespace and comments.
    for (;;) {
        while (peek(lexer) == ' ' || peek(lexer) == '\t' || peek(lexer) == '\r') {
            advance(lexer);
        }

        // Newlines are significant - emit TOKEN_NEWLINE as a statement
        // terminator.
        if (peek(lexer) == '\n') {
            SourceLocation location = current_location(lexer);
            advance(lexer); // consume '\n'
            lexer->line++;
            lexer->column = 1;
            return make_token(lexer, TOKEN_NEWLINE, "\n", 1, location);
        }

        // Line comments: skip until end of line.
        if (peek(lexer) == '/' && peek_next(lexer) == '/') {
            while (peek(lexer) != '\n' && peek(lexer) != '\0') {
                advance(lexer);
            }
            continue;
        }

        break;
    }

    SourceLocation location = current_location(lexer);
    char c = advance(lexer);

    if (c == '\0') {
        return make_token(lexer, TOKEN_EOF, "", 0, location);
    }
    if (is_digit(c)) {
        return scan_number(lexer, location);
    }
    if (is_alpha(c)) {
        return scan_ident(lexer, location);
    }
    if (c == '"') {
        return scan_string(lexer, location);
    }
    if (c == '\'') {
        return scan_char_literal(lexer, location);
    }
    return scan_punctuation(lexer, c, location);
}

Lexer *lexer_create(const char *source, const char *file, Arena *arena) {
    Lexer *lexer = rsg_malloc(sizeof(*lexer));
    lexer->source = source;
    lexer->file = file;
    lexer->position = 0;
    lexer->length = (int32_t)strlen(source);
    lexer->line = 1;
    lexer->column = 1;
    lexer->arena = arena;
    lexer->pending = NULL;
    lexer->pending_position = 0;
    lexer->last_kind = TOKEN_EOF;
    return lexer;
}

void lexer_destroy(Lexer *lexer) {
    if (lexer != NULL) {
        BUFFER_FREE(lexer->pending);
        free(lexer);
    }
}

Token lexer_next(Lexer *lexer) {
    // Return pending tokens first (from string interpolation)
    if (lexer->pending != NULL && lexer->pending_position < BUFFER_LENGTH(lexer->pending)) {
        Token token = lexer->pending[lexer->pending_position++];
        lexer->last_kind = token.kind;
        return token;
    }
    // Free pending buffer when exhausted
    if (lexer->pending != NULL) {
        BUFFER_FREE(lexer->pending);
        lexer->pending = NULL;
        lexer->pending_position = 0;
    }
    Token token = scan_token(lexer);
    lexer->last_kind = token.kind;
    return token;
}

Token *lexer_scan_all(Lexer *lexer) {
    Token *tokens = NULL;
    for (;;) {
        Token token = lexer_next(lexer);
        BUFFER_PUSH(tokens, token);
        if (token.kind == TOKEN_EOF) {
            break;
        }
    }
    return tokens;
}
