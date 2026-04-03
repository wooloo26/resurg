#include "lexer.h"

// ------------------------------------------------------------------------
// Private struct definition
// ------------------------------------------------------------------------
struct Lexer {
    const char *source;       // full source text
    const char *file;         // source file name (for diagnostics)
    int32_t position;         // current byte offset
    int32_t length;           // total source length
    int32_t line;             // current line (1-based)
    int32_t column;           // current column (1-based)
    Arena *arena;             // for allocating lexemes / string literals
    Token *pending; /* buf */ // for interpolation
    int32_t pending_position; // next index to return from pending
};

// ------------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------------
static bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool is_alnum(char c) {
    return is_alpha(c) || is_digit(c);
}

static char peek(const Lexer *l) {
    return (char)(l->position < l->length ? l->source[l->position] : '\0');
}

static char peek_next(const Lexer *l) {
    return (char)(l->position + 1 < l->length ? l->source[l->position + 1] : '\0');
}
static char advance(Lexer *l) {
    char c = peek(l);
    l->position++;
    l->column++;
    return c;
}

static bool match(Lexer *l, char expected) {
    if (peek(l) == expected) {
        advance(l);
        return true;
    }
    return false;
}

static SrcLoc current_location(const Lexer *l) {
    return (SrcLoc){.file = l->file, .line = l->line, .column = l->column};
}

static Token make_token(const Lexer *l, TokenKind kind, const char *start, int32_t length, SrcLoc s) {
    return (Token){
        .kind = kind,
        .lexeme = arena_strndup(l->arena, start, length),
        .length = length,
        .loc = s,
    };
}

// ------------------------------------------------------------------------
// Keyword table
// ------------------------------------------------------------------------
typedef struct {
    const char *word;
    TokenKind kind;
} Keyword;

static const Keyword KEYWORDS[] = {
    {"module", TOK_MODULE}, {"pub", TOK_PUB},   {"fn", TOK_FN},       {"var", TOK_VAR},     {"if", TOK_IF},
    {"else", TOK_ELSE},     {"loop", TOK_LOOP}, {"for", TOK_FOR},     {"break", TOK_BREAK}, {"continue", TOK_CONTINUE},
    {"assert", TOK_ASSERT}, {"true", TOK_TRUE}, {"false", TOK_FALSE}, {"bool", TOK_BOOL},   {"i32", TOK_I32},
    {"u32", TOK_U32},       {"f64", TOK_F64},   {"str", TOK_STR},     {"unit", TOK_UNIT},
};

static const int32_t KEYWORD_COUNT = (int32_t)(sizeof(KEYWORDS) / sizeof(KEYWORDS[0]));

static TokenKind lookup_keyword(const char *text, int32_t len) {
    for (int32_t i = 0; i < KEYWORD_COUNT; i++) {
        if ((int32_t)strlen(KEYWORDS[i].word) == len && memcmp(KEYWORDS[i].word, text, len) == 0) {
            return KEYWORDS[i].kind;
        }
    }
    return TOK_IDENT;
}

// ------------------------------------------------------------------------
// Scanning routines
// ------------------------------------------------------------------------
static Token scan_number(Lexer *l, SrcLoc s) {
    const char *start = l->source + l->position - 1;
    bool is_float = false;

    while (is_digit(peek(l)) || peek(l) == '_') {
        advance(l);
    }

    if (peek(l) == '.' && peek_next(l) != '.') {
        is_float = true;
        advance(l); // consume '.'
        while (is_digit(peek(l)) || peek(l) == '_') {
            advance(l);
        }
    }

    if (peek(l) == 'e' || peek(l) == 'E') {
        is_float = true;
        advance(l);
        if (peek(l) == '+' || peek(l) == '-') {
            advance(l);
        }
        while (is_digit(peek(l))) {
            advance(l);
        }
    }

    int32_t length = (int32_t)(l->source + l->position - start);
    Token t = make_token(l, is_float ? TOK_FLOAT_LIT : TOK_INT_LIT, start, length, s);

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
        t.lit.float_value = strtod(buffer, NULL);
    } else {
        t.lit.integer_value = strtoll(buffer, NULL, 10);
    }

    return t;
}

// Forward declaration for scanning a single non-string token
static Token scan_token(Lexer *l);

static char *extract_str_content(const Lexer *l, int32_t from, int32_t to) {
    // Copy raw content between positions (escape sequences preserved as-is)
    return arena_strndup(l->arena, l->source + from, to - from);
}

static Token make_str_token(const Lexer *l, const char *val, SrcLoc s) {
    (void)l;
    return (Token){
        .kind = TOK_STR_LIT,
        .lexeme = val,
        .length = (int32_t)strlen(val),
        .loc = s,
        .lit.string_value = (char *)val,
    };
}

static Token scan_string(Lexer *l, SrcLoc s) {
    // Opening '"' already consumed
    int32_t content_start = l->position;

    // Quick lookahead for interpolation (avoid save/restore)
    bool has_interp = false;
    for (int32_t i = l->position; i < l->length && l->source[i] != '"'; i++) {
        if (l->source[i] == '\\') {
            i++;
            continue;
        }
        if (l->source[i] == '{') {
            has_interp = true;
            break;
        }
    }

    if (!has_interp) {
        // Simple string: scan to closing quote
        while (peek(l) != '"' && peek(l) != '\0') {
            if (peek(l) == '\\') {
                advance(l);
            }
            if (peek(l) == '\n') {
                l->line++;
                l->column = 0;
            }
            advance(l);
        }

        if (peek(l) == '\0') {
            rg_error(s, "unterminated string literal");
            return make_token(l, TOK_ERROR, l->source + content_start - 1, l->position - content_start + 1, s);
        }

        char *val = extract_str_content(l, content_start, l->position);
        advance(l); // consume '"'
        return make_str_token(l, val, s);
    }

    // Interpolated string: produce token sequence into pending buffer
    // Pattern: STR_LIT [INTERP_START expr_tokens... INTERP_END STR_LIT]*
    l->pending = NULL;
    l->pending_position = 0;

    while (peek(l) != '"' && peek(l) != '\0') {
        // Scan text segment until '{' or '"'
        int32_t seg_start = l->position;
        SrcLoc seg_loc = current_location(l);

        while (peek(l) != '{' && peek(l) != '"' && peek(l) != '\0') {
            if (peek(l) == '\\') {
                advance(l);
            }
            if (peek(l) == '\n') {
                l->line++;
                l->column = 0;
            }
            advance(l);
        }

        // Emit text segment
        char *text = extract_str_content(l, seg_start, l->position);
        BUF_PUSH(l->pending, make_str_token(l, text, seg_loc));

        if (peek(l) == '{') {
            advance(l); // consume '{'

            // Emit INTERP_START
            BUF_PUSH(l->pending, make_token(l, TOK_INTERP_START, "{", 1, current_location(l)));

            // Lex expression tokens until matching '}'
            int32_t brace_depth = 1;
            while (brace_depth > 0 && peek(l) != '\0') {
                // Skip whitespace (but not newlines — those are inside a
                // string)
                while (peek(l) == ' ' || peek(l) == '\t') {
                    advance(l);
                }

                if (peek(l) == '}') {
                    brace_depth--;
                    if (brace_depth == 0) {
                        advance(l); // consume '}'
                        break;
                    }
                }
                if (peek(l) == '{') {
                    brace_depth++;
                }

                Token t = scan_token(l);
                if (t.kind == TOK_EOF || t.kind == TOK_ERROR) {
                    break;
                }
                // skip newlines inside interpolation
                if (t.kind == TOK_NEWLINE) {
                    continue;
                }
                BUF_PUSH(l->pending, t);
            }

            // Emit INTERP_END
            BUF_PUSH(l->pending, make_token(l, TOK_INTERP_END, "}", 1, current_location(l)));
        }
    }

    // If the string ends with an interpolation, emit a trailing empty STR_LIT
    // so the parser always sees: STR_LIT [INTERP_START expr INTERP_END STR_LIT]*
    if (BUF_LEN(l->pending) > 0 && l->pending[BUF_LEN(l->pending) - 1].kind == TOK_INTERP_END) {
        BUF_PUSH(l->pending, make_str_token(l, "", current_location(l)));
    }

    if (peek(l) == '\0') {
        rg_error(s, "unterminated string literal");
    } else {
        advance(l); // consume '"'
    }

    // Return first pending token
    if (BUF_LEN(l->pending) > 0) {
        Token first = l->pending[0];
        l->pending_position = 1;
        return first;
    }

    // Empty string edge case
    return make_str_token(l, "", s);
}

static Token scan_ident(Lexer *l, SrcLoc s) {
    const char *start = l->source + l->position - 1;
    while (is_alnum(peek(l))) {
        advance(l);
    }

    int32_t length = (int32_t)(l->source + l->position - start);
    TokenKind kind = lookup_keyword(start, length);
    return make_token(l, kind, start, length, s);
}

// ------------------------------------------------------------------------
// Punctuation and operator tokens
// ------------------------------------------------------------------------
static Token scan_punctuation(Lexer *l, char c, SrcLoc s) {
    switch (c) {
    case ':':
        return match(l, '=') ? make_token(l, TOK_COLON_EQ, ":=", 2, s) : make_token(l, TOK_COLON, ":", 1, s);
    case '=':
        return match(l, '=') ? make_token(l, TOK_EQ_EQ, "==", 2, s) : make_token(l, TOK_EQ, "=", 1, s);
    case '!':
        return match(l, '=') ? make_token(l, TOK_BANG_EQ, "!=", 2, s) : make_token(l, TOK_BANG, "!", 1, s);
    case '<':
        return match(l, '=') ? make_token(l, TOK_LT_EQ, "<=", 2, s) : make_token(l, TOK_LT, "<", 1, s);
    case '>':
        return match(l, '=') ? make_token(l, TOK_GT_EQ, ">=", 2, s) : make_token(l, TOK_GT, ">", 1, s);
    case '&':
        if (match(l, '&')) {
            return make_token(l, TOK_AMP_AMP, "&&", 2, s);
        }
        break;
    case '|':
        if (match(l, '|')) {
            return make_token(l, TOK_PIPE_PIPE, "||", 2, s);
        }
        return make_token(l, TOK_PIPE, "|", 1, s);
    case '+':
        return match(l, '=') ? make_token(l, TOK_PLUS_EQ, "+=", 2, s) : make_token(l, TOK_PLUS, "+", 1, s);
    case '-':
        if (match(l, '>')) {
            return make_token(l, TOK_ARROW, "->", 2, s);
        }
        return match(l, '=') ? make_token(l, TOK_MINUS_EQ, "-=", 2, s) : make_token(l, TOK_MINUS, "-", 1, s);
    case '*':
        return match(l, '=') ? make_token(l, TOK_STAR_EQ, "*=", 2, s) : make_token(l, TOK_STAR, "*", 1, s);
    case '/':
        return match(l, '=') ? make_token(l, TOK_SLASH_EQ, "/=", 2, s) : make_token(l, TOK_SLASH, "/", 1, s);
    case '%':
        return make_token(l, TOK_PERCENT, "%", 1, s);
    case '.':
        return match(l, '.') ? make_token(l, TOK_DOT_DOT, "..", 2, s) : make_token(l, TOK_DOT, ".", 1, s);
    case '(':
        return make_token(l, TOK_LPAREN, "(", 1, s);
    case ')':
        return make_token(l, TOK_RPAREN, ")", 1, s);
    case '{':
        return make_token(l, TOK_LBRACE, "{", 1, s);
    case '}':
        return make_token(l, TOK_RBRACE, "}", 1, s);
    case ',':
        return make_token(l, TOK_COMMA, ",", 1, s);
    case ';':
        return make_token(l, TOK_SEMICOLON, ";", 1, s);
    default:
        break;
    }
    rg_error(s, "unexpected character '%c'", c);
    return make_token(l, TOK_ERROR, &l->source[l->position - 1], 1, s);
}

// ------------------------------------------------------------------------
// Core token scanner — used by both lexer_next and interpolation scanning
// ------------------------------------------------------------------------
static Token scan_token(Lexer *l) {
    // Skip whitespace and comments
    for (;;) {
        while (peek(l) == ' ' || peek(l) == '\t' || peek(l) == '\r') {
            advance(l);
        }

        // Newlines (may be significant — emit TOK_NEWLINE)
        if (peek(l) == '\n') {
            SrcLoc s = current_location(l);
            advance(l);
            l->line++;
            l->column = 1;
            return make_token(l, TOK_NEWLINE, "\n", 1, s);
        }

        // Line comments
        if (peek(l) == '/' && peek_next(l) == '/') {
            while (peek(l) != '\n' && peek(l) != '\0') {
                advance(l);
            }
            continue;
        }

        break;
    }

    SrcLoc s = current_location(l);
    char c = advance(l);

    if (c == '\0') {
        return make_token(l, TOK_EOF, "", 0, s);
    }
    if (is_digit(c)) {
        return scan_number(l, s);
    }
    if (is_alpha(c)) {
        return scan_ident(l, s);
    }
    if (c == '"') {
        return scan_string(l, s);
    }
    return scan_punctuation(l, c, s);
}

// ------------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------------
Lexer *lexer_create(const char *source, const char *file, Arena *arena) {
    Lexer *l = malloc(sizeof(*l));
    if (l == NULL) {
        rg_fatal("out of memory");
    }
    l->source = source;
    l->file = file;
    l->position = 0;
    l->length = (int32_t)strlen(source);
    l->line = 1;
    l->column = 1;
    l->arena = arena;
    l->pending = NULL;
    l->pending_position = 0;
    return l;
}

void lexer_destroy(Lexer *l) {
    if (l != NULL) {
        BUF_FREE(l->pending);
        free(l);
    }
}

Token lexer_next(Lexer *l) {
    // Return pending tokens first (from string interpolation)
    if (l->pending != NULL && l->pending_position < BUF_LEN(l->pending)) {
        return l->pending[l->pending_position++];
    }
    // Free pending buffer when exhausted
    if (l->pending != NULL) {
        BUF_FREE(l->pending);
        l->pending = NULL;
        l->pending_position = 0;
    }
    return scan_token(l);
}

Token *lexer_scan_all(Lexer *l) {
    Token *tokens = NULL;
    for (;;) {
        Token t = lexer_next(l);
        BUF_PUSH(tokens, t);
        if (t.kind == TOK_EOF) {
            break;
        }
    }
    return tokens;
}
