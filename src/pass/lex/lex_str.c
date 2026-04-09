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

/** Encode a Unicode codepoint as UTF-8 into @p buf, return bytes written. */
static int32_t encode_utf8(uint32_t cp, char *buf) {
    if (cp <= 0x7F) {
        buf[0] = (char)cp;
        return 1;
    }
    if (cp <= 0x7FF) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp <= 0xFFFF) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp <= 0x10FFFF) {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    buf[0] = '?';
    return 1;
}

/** Copy str range, decoding escape sequences in-place. Writes decoded byte count to @p out_len. */
static char *decode_str_range(Lex *lex, int32_t from, int32_t to, int32_t *out_len) {
    int32_t raw_len = to - from;
    char *buf = arena_alloc(lex->arena, raw_len * 4 + 1);
    int32_t out = 0;
    for (int32_t i = from; i < to; i++) {
        if (lex->src[i] == '\\' && i + 1 < to) {
            i++; // consume '\\'
            switch (lex->src[i]) {
            case 'n':
                buf[out++] = '\n';
                break;
            case 't':
                buf[out++] = '\t';
                break;
            case '\\':
                buf[out++] = '\\';
                break;
            case '"':
                buf[out++] = '"';
                break;
            case '0':
                buf[out++] = '\0';
                break;
            case 'u':
                if (i + 1 < to && lex->src[i + 1] == '{') {
                    i += 2; // skip 'u{'
                    uint32_t cp = 0;
                    while (i < to && lex->src[i] != '}') {
                        char c = lex->src[i];
                        cp <<= 4;
                        if (c >= '0' && c <= '9') {
                            cp |= (uint32_t)(c - '0');
                        } else if (c >= 'a' && c <= 'f') {
                            cp |= (uint32_t)(c - 'a' + 10);
                        } else if (c >= 'A' && c <= 'F') {
                            cp |= (uint32_t)(c - 'A' + 10);
                        }
                        i++;
                    }
                    // i now points to '}' — loop increment will skip it
                    out += encode_utf8(cp, buf + out);
                } else {
                    buf[out++] = 'u'; // malformed, keep literal
                }
                break;
            default:
                // Unknown escape — keep backslash + char
                buf[out++] = '\\';
                buf[out++] = lex->src[i];
                break;
            }
        } else {
            buf[out++] = lex->src[i];
        }
    }
    buf[out] = '\0';
    *out_len = out;
    return buf;
}

static Token make_str_token(const Lex *lex, const char *value, int32_t byte_len, SrcLoc loc) {
    (void)lex;
    return (Token){
        .kind = TOKEN_STR_LIT,
        .lexeme = value,
        .len = byte_len,
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

    int32_t decoded_len = 0;
    char *value = decode_str_range(lex, content_start, lex->pos, &decoded_len);
    advance(lex); // consume '"'
    return make_str_token(lex, value, decoded_len, loc);
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

        while (peek(lex) != '"' && peek(lex) != '\0') {
            if (peek(lex) == '{') {
                break; // real interpolation
            }
            if (peek(lex) == '\\') {
                advance(lex); // consume '\\'
                // For \u{...}, skip the entire unicode escape so '{' is not
                // mistaken for interpolation
                if (peek(lex) == 'u' && lex->pos + 1 < lex->len && lex->src[lex->pos + 1] == '{') {
                    advance(lex); // consume 'u'
                    advance(lex); // consume '{'
                    while (peek(lex) != '}' && peek(lex) != '\0') {
                        advance(lex);
                    }
                    // advance below will consume '}'
                }
            }
            if (peek(lex) == '\n') {
                lex->line++;
                lex->column = 0;
            }
            advance(lex);
        }

        // Emit text segment
        int32_t seg_len = 0;
        char *text = decode_str_range(lex, segment_start, lex->pos, &seg_len);
        BUF_PUSH(lex->pending, make_str_token(lex, text, seg_len, segment_loc));

        if (peek(lex) == '{') {
            scan_interpolation_block(lex);
        }
    }

    // If the str ends with an interpolation, emit a trailing empty STR_LIT
    if (BUF_LEN(lex->pending) > 0 &&
        lex->pending[BUF_LEN(lex->pending) - 1].kind == TOKEN_INTERPOLATION_END) {
        BUF_PUSH(lex->pending, make_str_token(lex, "", 0, current_loc(lex)));
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
    return make_str_token(lex, "", 0, loc);
}

// ── Str dispatch ────────────────────────────────────────────────────

Token scan_str(Lex *lex, SrcLoc loc) {
    // Opening '"' already consumed
    int32_t content_start = lex->pos;

    // Quick lookahead for interpolation (avoid save/restore)
    bool has_interpolation = false;
    for (int32_t i = lex->pos; i < lex->len && lex->src[i] != '"'; i++) {
        if (lex->src[i] == '\\') {
            i++; // skip escaped char
            // For \u{...}, skip entire unicode escape
            if (i < lex->len && lex->src[i] == 'u' && i + 1 < lex->len && lex->src[i + 1] == '{') {
                i += 2; // skip 'u{'
                while (i < lex->len && lex->src[i] != '}') {
                    i++;
                }
                // i now at '}', loop increment skips past it
            }
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
