#include "_lexer.h"

// ── Character literal ──────────────────────────────────────────────────

Token scan_char_literal(Lexer *lexer, SourceLocation location) {
    // Opening '\'' already consumed
    uint32_t value;
    if (peek(lexer) == '\\') {
        advance(lexer); // consume '\\'
        char escaped = advance(lexer);
        switch (escaped) {
        case 'n':
            value = '\n';
            break;
        case 't':
            value = '\t';
            break;
        case '\\':
            value = '\\';
            break;
        case '\'':
            value = '\'';
            break;
        case '0':
            value = '\0';
            break;
        default:
            rsg_error(location, "unknown escape sequence '\\%c'", escaped);
            value = (unsigned char)escaped;
            break;
        }
    } else {
        value = (unsigned char)advance(lexer);
    }
    if (peek(lexer) != '\'') {
        rsg_error(location, "unterminated character literal");
        return make_token(lexer, TOKEN_ERROR, "'", 1, location);
    }
    advance(lexer); // consume closing '\''
    Token token = make_token(lexer, TOKEN_CHAR_LITERAL, "'", 1, location);
    token.literal_value.char_value = value;
    return token;
}

// ── String helpers ─────────────────────────────────────────────────────

static char *copy_string_range(const Lexer *lexer, int32_t from, int32_t to) {
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

/** Advance past a string body (handling escape sequences) until @p stop char or EOF. */
static void skip_string_body(Lexer *lexer, char stop) {
    while (peek(lexer) != stop && peek(lexer) != '\0') {
        if (peek(lexer) == '\\') {
            advance(lexer); // consume '\\'
        }
        if (peek(lexer) == '\n') {
            lexer->line++;
            lexer->column = 0;
        }
        advance(lexer);
    }
}

// ── Simple strings ─────────────────────────────────────────────────────

/** Scan a simple (non-interpolated) string literal body. */
static Token scan_simple_string(Lexer *lexer, int32_t content_start, SourceLocation location) {
    skip_string_body(lexer, '"');

    if (peek(lexer) == '\0') {
        rsg_error(location, "unterminated string literal");
        return make_token(lexer, TOKEN_ERROR, lexer->source + content_start - 1,
                          lexer->position - content_start + 1, location);
    }

    char *value = copy_string_range(lexer, content_start, lexer->position);
    advance(lexer); // consume '"'
    return make_string_token(lexer, value, location);
}

// ── Interpolated strings ───────────────────────────────────────────────

/**
 * Lex the expression tokens inside a single `{...}` interpolation block.
 * Appends tokens to @c lexer->pending and advances past the closing `}`.
 */
static void scan_interpolation_block(Lexer *lexer) {
    advance(lexer); // consume '{'

    Token interp_start =
        make_token(lexer, TOKEN_INTERPOLATION_START, "{", 1, current_location(lexer));
    BUFFER_PUSH(lexer->pending, interp_start);

    int32_t brace_depth = 1;
    while (brace_depth > 0 && peek(lexer) != '\0') {
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
        if (token.kind == TOKEN_NEWLINE) {
            continue;
        }
        BUFFER_PUSH(lexer->pending, token);
    }

    Token interp_end = make_token(lexer, TOKEN_INTERPOLATION_END, "}", 1, current_location(lexer));
    BUFFER_PUSH(lexer->pending, interp_end);
}

/** Scan an interpolated string, populating lexer->pending with token sequence. */
static Token scan_interpolated_string(Lexer *lexer, SourceLocation location) {
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
        char *text = copy_string_range(lexer, segment_start, lexer->position);
        BUFFER_PUSH(lexer->pending, make_string_token(lexer, text, segment_location));

        if (peek(lexer) == '{') {
            scan_interpolation_block(lexer);
        }
    }

    // If the string ends with an interpolation, emit a trailing empty STRING_LITERAL
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

// ── String dispatch ────────────────────────────────────────────────────

Token scan_string(Lexer *lexer, SourceLocation location) {
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
        return scan_simple_string(lexer, content_start, location);
    }
    return scan_interpolated_string(lexer, location);
}
