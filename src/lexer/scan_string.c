#include "_lexer.h"

// ── Character lit ──────────────────────────────────────────────────

Token scan_char_lit(Lexer *lexer, SourceLoc loc) {
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
            rsg_err(loc, "unknown escape sequence '\\%c'", escaped);
            value = (unsigned char)escaped;
            break;
        }
    } else {
        value = (unsigned char)advance(lexer);
    }
    if (peek(lexer) != '\'') {
        rsg_err(loc, "unterminated character lit");
        return make_token(lexer, TOKEN_ERR, "'", 1, loc);
    }
    advance(lexer); // consume closing '\''
    Token token = make_token(lexer, TOKEN_CHAR_LIT, "'", 1, loc);
    token.lit_value.char_value = value;
    return token;
}

// ── Str helpers ─────────────────────────────────────────────────────

static char *copy_str_range(const Lexer *lexer, int32_t from, int32_t to) {
    // Copy raw content between poss (escape sequences preserved as-is)
    return arena_strndup(lexer->arena, lexer->source + from, to - from);
}

static Token make_str_token(const Lexer *lexer, const char *value, SourceLoc loc) {
    (void)lexer;
    return (Token){
        .kind = TOKEN_STR_LIT,
        .lexeme = value,
        .len = (int32_t)strlen(value),
        .loc = loc,
        .lit_value.str_value = (char *)value,
    };
}

/** Advance past a str body (handling escape sequences) until @p stop char or EOF. */
static void skip_str_body(Lexer *lexer, char stop) {
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

// ── Simple strs ─────────────────────────────────────────────────────

/** Scan a simple (non-interpolated) str lit body. */
static Token scan_simple_str(Lexer *lexer, int32_t content_start, SourceLoc loc) {
    skip_str_body(lexer, '"');

    if (peek(lexer) == '\0') {
        rsg_err(loc, "unterminated str lit");
        return make_token(lexer, TOKEN_ERR, lexer->source + content_start - 1,
                          lexer->pos - content_start + 1, loc);
    }

    char *value = copy_str_range(lexer, content_start, lexer->pos);
    advance(lexer); // consume '"'
    return make_str_token(lexer, value, loc);
}

// ── Interpolated strs ───────────────────────────────────────────────

/**
 * Lex the expr tokens inside a single `{...}` interpolation block.
 * Appends tokens to @c lexer->pending and advances past the closing `}`.
 */
static void scan_interpolation_block(Lexer *lexer) {
    advance(lexer); // consume '{'

    Token interp_start = make_token(lexer, TOKEN_INTERPOLATION_START, "{", 1, current_loc(lexer));
    BUF_PUSH(lexer->pending, interp_start);

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
        if (token.kind == TOKEN_EOF || token.kind == TOKEN_ERR) {
            break;
        }
        if (token.kind == TOKEN_NEWLINE) {
            continue;
        }
        BUF_PUSH(lexer->pending, token);
    }

    Token interp_end = make_token(lexer, TOKEN_INTERPOLATION_END, "}", 1, current_loc(lexer));
    BUF_PUSH(lexer->pending, interp_end);
}

/** Scan an interpolated str, populating lexer->pending with token sequence. */
static Token scan_interpolated_str(Lexer *lexer, SourceLoc loc) {
    lexer->pending = NULL;
    lexer->pending_pos = 0;

    while (peek(lexer) != '"' && peek(lexer) != '\0') {
        // Scan text segment until '{' or '"'
        int32_t segment_start = lexer->pos;
        SourceLoc segment_loc = current_loc(lexer);

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
        char *text = copy_str_range(lexer, segment_start, lexer->pos);
        BUF_PUSH(lexer->pending, make_str_token(lexer, text, segment_loc));

        if (peek(lexer) == '{') {
            scan_interpolation_block(lexer);
        }
    }

    // If the str ends with an interpolation, emit a trailing empty STR_LIT
    if (BUF_LEN(lexer->pending) > 0 &&
        lexer->pending[BUF_LEN(lexer->pending) - 1].kind == TOKEN_INTERPOLATION_END) {
        BUF_PUSH(lexer->pending, make_str_token(lexer, "", current_loc(lexer)));
    }

    if (peek(lexer) == '\0') {
        rsg_err(loc, "unterminated str lit");
    } else {
        advance(lexer); // consume '"'
    }

    // Return first pending token
    if (BUF_LEN(lexer->pending) > 0) {
        Token first = lexer->pending[0];
        lexer->pending_pos = 1;
        return first;
    }

    // Empty str edge case
    return make_str_token(lexer, "", loc);
}

// ── Str dispatch ────────────────────────────────────────────────────

Token scan_str(Lexer *lexer, SourceLoc loc) {
    // Opening '"' already consumed
    int32_t content_start = lexer->pos;

    // Quick lookahead for interpolation (avoid save/restore)
    bool has_interpolation = false;
    for (int32_t i = lexer->pos; i < lexer->len && lexer->source[i] != '"'; i++) {
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
        return scan_simple_str(lexer, content_start, loc);
    }
    return scan_interpolated_str(lexer, loc);
}
