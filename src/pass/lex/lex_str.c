#include "_lex.h"

// ── Character lit ──────────────────────────────────────────────────

Token scan_char_lit(Lex *lex, SrcLoc loc) {
    // Opening '\'' already consumed
    uint32_t value;
    if (peek(lex) == '\\') {
        advance(lex); // consume '\\'
        char escaped = advance(lex);
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
        value = (unsigned char)advance(lex);
    }
    if (peek(lex) != '\'') {
        rsg_err(loc, "unterminated character lit");
        return build_token(lex, TOKEN_ERR, "'", 1, loc);
    }
    advance(lex); // consume closing '\''
    Token token = build_token(lex, TOKEN_CHAR_LIT, "'", 1, loc);
    token.lit_value.char_value = value;
    return token;
}

// ── Str helpers ─────────────────────────────────────────────────────

static char *copy_str_range(const Lex *lex, int32_t from, int32_t to) {
    // Copy raw content between poss (escape sequences preserved as-is)
    return arena_strndup(lex->arena, lex->src + from, to - from);
}

static Token make_str_token(const Lex *lex, const char *value, SrcLoc loc) {
    (void)lex;
    return (Token){
        .kind = TOKEN_STR_LIT,
        .lexeme = value,
        .len = (int32_t)strlen(value),
        .loc = loc,
        .lit_value.str_value = (char *)value,
    };
}

/** Advance past a str body (handling escape sequences) until @p stop char or EOF. */
static void skip_str_body(Lex *lex, char stop) {
    while (peek(lex) != stop && peek(lex) != '\0') {
        if (peek(lex) == '\\') {
            advance(lex); // consume '\\'
        }
        if (peek(lex) == '\n') {
            lex->line++;
            lex->column = 0;
        }
        advance(lex);
    }
}

// ── Simple strs ─────────────────────────────────────────────────────

/** Scan a simple (non-interpolated) str lit body. */
static Token scan_simple_str(Lex *lex, int32_t content_start, SrcLoc loc) {
    skip_str_body(lex, '"');

    if (peek(lex) == '\0') {
        rsg_err(loc, "unterminated str lit");
        return build_token(lex, TOKEN_ERR, lex->src + content_start - 1,
                           lex->pos - content_start + 1, loc);
    }

    char *value = copy_str_range(lex, content_start, lex->pos);
    advance(lex); // consume '"'
    return make_str_token(lex, value, loc);
}

// ── Interpolated strs ───────────────────────────────────────────────

/**
 * Lex the expr tokens inside a single `{...}` interpolation block.
 * Appends tokens to @c lex->pending and advances past the closing `}`.
 */
static void scan_interpolation_block(Lex *lex) {
    advance(lex); // consume '{'

    Token interp_start = build_token(lex, TOKEN_INTERPOLATION_START, "{", 1, current_loc(lex));
    BUF_PUSH(lex->pending, interp_start);

    int32_t brace_depth = 1;
    while (brace_depth > 0 && peek(lex) != '\0') {
        while (peek(lex) == ' ' || peek(lex) == '\t') {
            advance(lex);
        }

        if (peek(lex) == '}') {
            brace_depth--;
            if (brace_depth == 0) {
                advance(lex); // consume '}'
                break;
            }
        }
        if (peek(lex) == '{') {
            brace_depth++;
        }

        Token token = scan_token(lex);
        if (token.kind == TOKEN_EOF || token.kind == TOKEN_ERR) {
            break;
        }
        if (token.kind == TOKEN_NEWLINE) {
            continue;
        }
        BUF_PUSH(lex->pending, token);
    }

    Token interp_end = build_token(lex, TOKEN_INTERPOLATION_END, "}", 1, current_loc(lex));
    BUF_PUSH(lex->pending, interp_end);
}

/** Scan an interpolated str, populating lex->pending with token sequence. */
static Token scan_interpolated_str(Lex *lex, SrcLoc loc) {
    lex->pending = NULL;
    lex->pending_pos = 0;

    while (peek(lex) != '"' && peek(lex) != '\0') {
        // Scan text segment until '{' or '"'
        int32_t segment_start = lex->pos;
        SrcLoc segment_loc = current_loc(lex);

        while (peek(lex) != '{' && peek(lex) != '"' && peek(lex) != '\0') {
            if (peek(lex) == '\\') {
                advance(lex); // consume '\\'
            }
            if (peek(lex) == '\n') {
                lex->line++;
                lex->column = 0;
            }
            advance(lex);
        }

        // Emit text segment
        char *text = copy_str_range(lex, segment_start, lex->pos);
        BUF_PUSH(lex->pending, make_str_token(lex, text, segment_loc));

        if (peek(lex) == '{') {
            scan_interpolation_block(lex);
        }
    }

    // If the str ends with an interpolation, emit a trailing empty STR_LIT
    if (BUF_LEN(lex->pending) > 0 &&
        lex->pending[BUF_LEN(lex->pending) - 1].kind == TOKEN_INTERPOLATION_END) {
        BUF_PUSH(lex->pending, make_str_token(lex, "", current_loc(lex)));
    }

    if (peek(lex) == '\0') {
        rsg_err(loc, "unterminated str lit");
    } else {
        advance(lex); // consume '"'
    }

    // Return first pending token
    if (BUF_LEN(lex->pending) > 0) {
        Token first = lex->pending[0];
        lex->pending_pos = 1;
        return first;
    }

    // Empty str edge case
    return make_str_token(lex, "", loc);
}

// ── Str dispatch ────────────────────────────────────────────────────

Token scan_str(Lex *lex, SrcLoc loc) {
    // Opening '"' already consumed
    int32_t content_start = lex->pos;

    // Quick lookahead for interpolation (avoid save/restore)
    bool has_interpolation = false;
    for (int32_t i = lex->pos; i < lex->len && lex->src[i] != '"'; i++) {
        if (lex->src[i] == '\\') {
            i++;
            continue;
        }
        if (lex->src[i] == '{') {
            has_interpolation = true;
            break;
        }
    }

    if (!has_interpolation) {
        return scan_simple_str(lex, content_start, loc);
    }
    return scan_interpolated_str(lex, loc);
}
