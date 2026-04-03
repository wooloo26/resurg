#include "lexer.h"

struct Lexer {
    const char *source;
    const char *file; // source filename for diagnostics
    int32_t position;
    int32_t length;
    int32_t line;             // 1-based current line
    int32_t column;           // 1-based current column
    Arena *arena;             // for allocating lexemes / string literals
    Token *pending; /* buf */ // buffered tokens from string interpolation
    int32_t pending_position; // read cursor into pending
};

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

static char peek(const Lexer *lexer) {
    return (char)(lexer->position < lexer->length ? lexer->source[lexer->position] : '\0');
}

static char peek_next(const Lexer *lexer) {
    return (char)(lexer->position + 1 < lexer->length ? lexer->source[lexer->position + 1] : '\0');
}
static char advance(Lexer *lexer) {
    char c = peek(lexer);
    lexer->position++;
    lexer->column++;
    return c;
}

static bool match(Lexer *lexer, char expected) {
    if (peek(lexer) == expected) {
        advance(lexer);
        return true;
    }
    return false;
}

static SourceLocation current_location(const Lexer *lexer) {
    return (SourceLocation){.file = lexer->file, .line = lexer->line, .column = lexer->column};
}

static Token make_token(const Lexer *lexer, TokenKind kind, const char *start, int32_t length,
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
    {"module", TOKEN_MODULE}, {"pub", TOKEN_PUBLIC},        {"fn", TOKEN_FUNCTION}, {"var", TOKEN_VARIABLE},
    {"if", TOKEN_IF},         {"else", TOKEN_ELSE},         {"loop", TOKEN_LOOP},   {"for", TOKEN_FOR},
    {"break", TOKEN_BREAK},   {"continue", TOKEN_CONTINUE}, {"true", TOKEN_TRUE},   {"false", TOKEN_FALSE},
    {"bool", TOKEN_BOOL},     {"i32", TOKEN_I32},           {"u32", TOKEN_U32},     {"f64", TOKEN_F64},
    {"str", TOKEN_STRING},    {"unit", TOKEN_UNIT},
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

    if (peek(lexer) == '.' && peek_next(lexer) != '.') {
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
    Token token = make_token(lexer, is_float ? TOKEN_FLOAT_LITERAL : TOKEN_INTEGER_LITERAL, start, length, location);

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
        token.literal_value.integer_value = strtoll(buffer, NULL, 10);
    }

    return token;
}

static Token scan_token(Lexer *l);

static char *extract_string_content(const Lexer *lexer, int32_t from, int32_t to) {
    // Copy raw content between positions (escape sequences preserved as-is)
    return arena_strndup(lexer->arena, lexer->source + from, to - from);
}

static Token make_string_token(const Lexer *lexer, const char *value, SourceLocation location) {
    (void)lexer;
    return (Token){
        .kind = TOKEN_STRING_LITERAL,
        .lexeme = value,
        .length = (int32_t)strlen(value),
        .location = location,
        .literal_value.string_value = (char *)value,
    };
}

static Token scan_string(Lexer *lexer, SourceLocation location) {
    // Opening '"' already consumed
    int32_t content_start = lexer->position;

    // Quick lookahead for interpolation (avoid save/restore)
    bool has_interpolation = false;
    for (int32_t i = lexer->position; i < lexer->length && lexer->source[i] != '"'; i++) {
        if (lexer->source[i] == '\\') {
            i++;
            continue;
        }
        if (lexer->source[i] == '{') {
            has_interpolation = true;
            break;
        }
    }

    if (!has_interpolation) {
        // Simple string: scan to closing quote
        while (peek(lexer) != '"' && peek(lexer) != '\0') {
            if (peek(lexer) == '\\') {
                advance(lexer); // consume '\\'
            }
            if (peek(lexer) == '\n') {
                lexer->line++;
                lexer->column = 0;
            }
            advance(lexer);
        }

        if (peek(lexer) == '\0') {
            rsg_error(location, "unterminated string literal");
            return make_token(lexer, TOKEN_ERROR, lexer->source + content_start - 1,
                              lexer->position - content_start + 1, location);
        }

        char *value = extract_string_content(lexer, content_start, lexer->position);
        advance(lexer); // consume '"'
        return make_string_token(lexer, value, location);
    }

    // Interpolated string: produce token sequence into pending buffer
    // Pattern: STRING_LITERAL [INTERPOLATION_START expr_tokens... INTERPOLATION_END STRING_LITERAL]*
    lexer->pending = NULL;
    lexer->pending_position = 0;

    while (peek(lexer) != '"' && peek(lexer) != '\0') {
        // Scan text segment until '{' or '"'
        int32_t segment_start = lexer->position;
        SourceLocation segment_location = current_location(lexer);

        while (peek(lexer) != '{' && peek(lexer) != '"' && peek(lexer) != '\0') {
            if (peek(lexer) == '\\') {
                advance(lexer); // consume '\\'
            }
            if (peek(lexer) == '\n') {
                lexer->line++;
                lexer->column = 0;
            }
            advance(lexer);
        }

        // Emit text segment
        char *text = extract_string_content(lexer, segment_start, lexer->position);
        BUFFER_PUSH(lexer->pending, make_string_token(lexer, text, segment_location));

        if (peek(lexer) == '{') {
            advance(lexer); // consume '{'

            // Emit INTERPOLATION_START
            BUFFER_PUSH(lexer->pending, make_token(lexer, TOKEN_INTERPOLATION_START, "{", 1, current_location(lexer)));

            // Lex expression tokens until matching '}'
            int32_t brace_depth = 1;
            while (brace_depth > 0 && peek(lexer) != '\0') {
                // Skip whitespace (but not newlines - those are inside a
                // string)
                while (peek(lexer) == ' ' || peek(lexer) == '\t') {
                    advance(lexer);
                }

                if (peek(lexer) == '}') {
                    brace_depth--;
                    if (brace_depth == 0) {
                        advance(lexer); // consume '}'
                        break;
                    }
                }
                if (peek(lexer) == '{') {
                    brace_depth++;
                }

                Token token = scan_token(lexer);
                if (token.kind == TOKEN_EOF || token.kind == TOKEN_ERROR) {
                    break;
                }
                // skip newlines inside interpolation
                if (token.kind == TOKEN_NEWLINE) {
                    continue;
                }
                BUFFER_PUSH(lexer->pending, token);
            }

            // Emit INTERPOLATION_END
            BUFFER_PUSH(lexer->pending, make_token(lexer, TOKEN_INTERPOLATION_END, "}", 1, current_location(lexer)));
        }
    }

    // If the string ends with an interpolation, emit a trailing empty STRING_LITERAL
    // so the parser always sees: STRING_LITERAL [INTERPOLATION_START expr INTERPOLATION_END STRING_LITERAL]*
    if (BUFFER_LENGTH(lexer->pending) > 0 &&
        lexer->pending[BUFFER_LENGTH(lexer->pending) - 1].kind == TOKEN_INTERPOLATION_END) {
        BUFFER_PUSH(lexer->pending, make_string_token(lexer, "", current_location(lexer)));
    }

    if (peek(lexer) == '\0') {
        rsg_error(location, "unterminated string literal");
    } else {
        advance(lexer); // consume '"'
    }

    // Return first pending token
    if (BUFFER_LENGTH(lexer->pending) > 0) {
        Token first = lexer->pending[0];
        lexer->pending_position = 1;
        return first;
    }

    // Empty string edge case
    return make_string_token(lexer, "", location);
}

static Token scan_ident(Lexer *lexer, SourceLocation location) {
    const char *start = lexer->source + lexer->position - 1;
    while (is_alnum(peek(lexer))) {
        advance(lexer);
    }

    int32_t length = (int32_t)(lexer->source + lexer->position - start);
    TokenKind kind = lookup_keyword(start, length);
    return make_token(lexer, kind, start, length, location);
}

/** Dispatch a single- or double-character punctuation / operator token. */
static Token scan_punctuation(Lexer *lexer, char c, SourceLocation location) {
    switch (c) {
    case ':':
        return match(lexer, '=') ? make_token(lexer, TOKEN_COLON_EQUAL, ":=", 2, location)
                                 : make_token(lexer, TOKEN_COLON, ":", 1, location);
    case '=':
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
        break;
    case '|':
        if (match(lexer, '|')) {
            return make_token(lexer, TOKEN_PIPE_PIPE, "||", 2, location);
        }
        return make_token(lexer, TOKEN_PIPE, "|", 1, location);
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
    case '.':
        return match(lexer, '.') ? make_token(lexer, TOKEN_DOT_DOT, "..", 2, location)
                                 : make_token(lexer, TOKEN_DOT, ".", 1, location);
    case '(':
        return make_token(lexer, TOKEN_LEFT_PAREN, "(", 1, location);
    case ')':
        return make_token(lexer, TOKEN_RIGHT_PAREN, ")", 1, location);
    case '{':
        return make_token(lexer, TOKEN_LEFT_BRACE, "{", 1, location);
    case '}':
        return make_token(lexer, TOKEN_RIGHT_BRACE, "}", 1, location);
    case ',':
        return make_token(lexer, TOKEN_COMMA, ",", 1, location);
    case ';':
        return make_token(lexer, TOKEN_SEMICOLON, ";", 1, location);
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
static Token scan_token(Lexer *lexer) {
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
    return scan_punctuation(lexer, c, location);
}

Lexer *lexer_create(const char *source, const char *file, Arena *arena) {
    Lexer *lexer = malloc(sizeof(*lexer));
    if (lexer == NULL) {
        rsg_fatal("out of memory");
    }
    lexer->source = source;
    lexer->file = file;
    lexer->position = 0;
    lexer->length = (int32_t)strlen(source);
    lexer->line = 1;
    lexer->column = 1;
    lexer->arena = arena;
    lexer->pending = NULL;
    lexer->pending_position = 0;
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
        return lexer->pending[lexer->pending_position++];
    }
    // Free pending buffer when exhausted
    if (lexer->pending != NULL) {
        BUFFER_FREE(lexer->pending);
        lexer->pending = NULL;
        lexer->pending_position = 0;
    }
    return scan_token(lexer);
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
