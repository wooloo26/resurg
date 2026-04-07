#include "_lex.h"

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

Token build_token(const Lex *lex, TokenKind kind, const char *start, int32_t len, SrcLoc loc) {
    return (Token){
        .kind = kind,
        .lexeme = arena_strndup(lex->arena, start, len),
        .len = len,
        .loc = loc,
    };
}

/** Keyword lookup table - maps reserved words to their TokenKind. */
typedef struct {
    const char *word;
    TokenKind kind;
} Keyword;

static const Keyword KEYWORDS[] = {
    {"module", TOKEN_MODULE},     {"pub", TOKEN_PUB},       {"fn", TOKEN_FN},
    {"var", TOKEN_VAR},           {"if", TOKEN_IF},         {"else", TOKEN_ELSE},
    {"loop", TOKEN_LOOP},         {"for", TOKEN_FOR},       {"break", TOKEN_BREAK},
    {"continue", TOKEN_CONTINUE}, {"true", TOKEN_TRUE},     {"false", TOKEN_FALSE},
    {"type", TOKEN_TYPE},         {"struct", TOKEN_STRUCT}, {"mut", TOKEN_MUT},
    {"immut", TOKEN_IMMUT},       {"enum", TOKEN_ENUM},     {"pact", TOKEN_PACT},
    {"match", TOKEN_MATCH},       {"return", TOKEN_RETURN}, {"while", TOKEN_WHILE},
    {"defer", TOKEN_DEFER},       {"bool", TOKEN_BOOL},     {"i8", TOKEN_I8},
    {"i16", TOKEN_I16},           {"i32", TOKEN_I32},       {"i64", TOKEN_I64},
    {"i128", TOKEN_I128},         {"u8", TOKEN_U8},         {"u16", TOKEN_U16},
    {"u32", TOKEN_U32},           {"u64", TOKEN_U64},       {"u128", TOKEN_U128},
    {"isize", TOKEN_ISIZE},       {"usize", TOKEN_USIZE},   {"f32", TOKEN_F32},
    {"f64", TOKEN_F64},           {"char", TOKEN_CHAR},     {"str", TOKEN_STR},
    {"unit", TOKEN_UNIT},         {"never", TOKEN_NEVER},
};

static const int32_t KEYWORD_COUNT = (int32_t)(sizeof(KEYWORDS) / sizeof(KEYWORDS[0]));

/**
 * Look up @p text (len @p len) in the keyword table.  Returns
 * TOKEN_ID if no keyword matches.
 */
static TokenKind lookup_keyword(const char *text, int32_t len) {
    for (int32_t i = 0; i < KEYWORD_COUNT; i++) {
        if ((int32_t)strlen(KEYWORDS[i].word) == len && memcmp(KEYWORDS[i].word, text, len) == 0) {
            return KEYWORDS[i].kind;
        }
    }
    return TOKEN_ID;
}

// Scanning routines - one per lexeme category.

/**
 * Scan a decimal integer or floating-point lit.  Underscores are
 * silently stripped before parsing the numeric value.
 */
static Token scan_number(Lex *lex, SrcLoc loc) {
    const char *start = lex->src + lex->pos - 1;
    bool is_float = false;

    while (is_digit(peek(lex)) || peek(lex) == '_') {
        advance(lex);
    }

    if (peek(lex) == '.' && peek_next(lex) != '.' && lex->last_kind != TOKEN_DOT) {
        is_float = true;
        advance(lex); // consume '.'
        while (is_digit(peek(lex)) || peek(lex) == '_') {
            advance(lex);
        }
    }

    if (peek(lex) == 'e' || peek(lex) == 'E') {
        is_float = true;
        advance(lex); // consume 'e' or 'E'
        if (peek(lex) == '+' || peek(lex) == '-') {
            advance(lex); // consume sign
        }
        while (is_digit(peek(lex))) {
            advance(lex);
        }
    }

    int32_t len = (int32_t)(lex->src + lex->pos - start);
    TokenKind number_kind = is_float ? TOKEN_FLOAT_LIT : TOKEN_INTEGER_LIT;
    Token token = build_token(lex, number_kind, start, len, loc);

    // Strip underscores and parse value
    char buf[64];
    int32_t buf_idx = 0;
    for (int32_t i = 0; i < len && buf_idx < 63; i++) {
        if (start[i] != '_') {
            buf[buf_idx++] = start[i];
        }
    }
    buf[buf_idx] = '\0';

    if (is_float) {
        token.lit_value.float_value = strtod(buf, NULL);
    } else {
        token.lit_value.integer_value = strtoull(buf, NULL, 10);
    }

    return token;
}

// scan_char_lit and scan_str are in scan_str.c

/** Reserved keywords that cannot be used as ids. */
static const char *const RESERVED_KEYWORDS[] = {
    "async", "await", "comptime", "macro", "spawn", "use", "where",
};

static const int32_t RESERVED_KEYWORD_COUNT =
    (int32_t)(sizeof(RESERVED_KEYWORDS) / sizeof(RESERVED_KEYWORDS[0]));

/** Return true if @p text (len @p len) is a reserved keyword. */
static bool is_reserved_keyword(const char *text, int32_t len) {
    for (int32_t i = 0; i < RESERVED_KEYWORD_COUNT; i++) {
        if ((int32_t)strlen(RESERVED_KEYWORDS[i]) == len &&
            memcmp(RESERVED_KEYWORDS[i], text, len) == 0) {
            return true;
        }
    }
    return false;
}

static Token scan_ident(Lex *lex, SrcLoc loc) {
    const char *start = lex->src + lex->pos - 1;
    while (is_alnum(peek(lex))) {
        advance(lex);
    }

    int32_t len = (int32_t)(lex->src + lex->pos - start);
    TokenKind kind = lookup_keyword(start, len);

    if (kind == TOKEN_ID && is_reserved_keyword(start, len)) {
        rsg_err(loc, "'%.*s' is a reserved keyword", len, start);
        return build_token(lex, TOKEN_ERR, start, len, loc);
    }

    return build_token(lex, kind, start, len, loc);
}

/** Dispatch a comparison or logical operator token. */
static Token scan_comparison_or_logical(Lex *lex, char c, SrcLoc loc) {
    switch (c) {
    case '=':
        if (match(lex, '>')) {
            return build_token(lex, TOKEN_FAT_ARROW, "=>", 2, loc);
        }
        return match(lex, '=') ? build_token(lex, TOKEN_EQUAL_EQUAL, "==", 2, loc)
                               : build_token(lex, TOKEN_EQUAL, "=", 1, loc);
    case '!':
        return match(lex, '=') ? build_token(lex, TOKEN_BANG_EQUAL, "!=", 2, loc)
                               : build_token(lex, TOKEN_BANG, "!", 1, loc);
    case '<':
        return match(lex, '=') ? build_token(lex, TOKEN_LESS_EQUAL, "<=", 2, loc)
                               : build_token(lex, TOKEN_LESS, "<", 1, loc);
    case '>':
        return match(lex, '=') ? build_token(lex, TOKEN_GREATER_EQUAL, ">=", 2, loc)
                               : build_token(lex, TOKEN_GREATER, ">", 1, loc);
    case '&':
        if (match(lex, '&')) {
            return build_token(lex, TOKEN_AMPERSAND_AMPERSAND, "&&", 2, loc);
        }
        return build_token(lex, TOKEN_AMPERSAND, "&", 1, loc);
    case '|':
        if (match(lex, '|')) {
            return build_token(lex, TOKEN_PIPE_PIPE, "||", 2, loc);
        }
        if (match(lex, '>')) {
            return build_token(lex, TOKEN_PIPE_GREATER, "|>", 2, loc);
        }
        return build_token(lex, TOKEN_PIPE, "|", 1, loc);
    default:
        break;
    }
    return build_token(lex, TOKEN_ERR, &lex->src[lex->pos - 1], 1, loc);
}

/** Dispatch an arithmetic operator token (with optional compound assignment). */
static Token scan_arithmetic(Lex *lex, char c, SrcLoc loc) {
    switch (c) {
    case '+':
        return match(lex, '=') ? build_token(lex, TOKEN_PLUS_EQUAL, "+=", 2, loc)
                               : build_token(lex, TOKEN_PLUS, "+", 1, loc);
    case '-':
        if (match(lex, '>')) {
            return build_token(lex, TOKEN_ARROW, "->", 2, loc);
        }
        return match(lex, '=') ? build_token(lex, TOKEN_MINUS_EQUAL, "-=", 2, loc)
                               : build_token(lex, TOKEN_MINUS, "-", 1, loc);
    case '*':
        return match(lex, '=') ? build_token(lex, TOKEN_STAR_EQUAL, "*=", 2, loc)
                               : build_token(lex, TOKEN_STAR, "*", 1, loc);
    case '/':
        return match(lex, '=') ? build_token(lex, TOKEN_SLASH_EQUAL, "/=", 2, loc)
                               : build_token(lex, TOKEN_SLASH, "/", 1, loc);
    case '%':
        return build_token(lex, TOKEN_PERCENT, "%", 1, loc);
    default:
        break;
    }
    return build_token(lex, TOKEN_ERR, &lex->src[lex->pos - 1], 1, loc);
}

/** Dispatch a single- or double-character punctuation / operator token. */
static Token scan_punctuation(Lex *lex, char c, SrcLoc loc) {
    switch (c) {
    case ':':
        if (match(lex, ':')) {
            return build_token(lex, TOKEN_COLON_COLON, "::", 2, loc);
        }
        return match(lex, '=') ? build_token(lex, TOKEN_COLON_EQUAL, ":=", 2, loc)
                               : build_token(lex, TOKEN_COLON, ":", 1, loc);
    case '.':
        if (match(lex, '.')) {
            return match(lex, '=') ? build_token(lex, TOKEN_DOT_DOT_EQUAL, "..=", 3, loc)
                                   : build_token(lex, TOKEN_DOT_DOT, "..", 2, loc);
        }
        return build_token(lex, TOKEN_DOT, ".", 1, loc);
    case '(':
        return build_token(lex, TOKEN_LEFT_PAREN, "(", 1, loc);
    case ')':
        return build_token(lex, TOKEN_RIGHT_PAREN, ")", 1, loc);
    case '{':
        return build_token(lex, TOKEN_LEFT_BRACE, "{", 1, loc);
    case '}':
        return build_token(lex, TOKEN_RIGHT_BRACE, "}", 1, loc);
    case '[':
        return build_token(lex, TOKEN_LEFT_BRACKET, "[", 1, loc);
    case ']':
        return build_token(lex, TOKEN_RIGHT_BRACKET, "]", 1, loc);
    case ',':
        return build_token(lex, TOKEN_COMMA, ",", 1, loc);
    case ';':
        return build_token(lex, TOKEN_SEMICOLON, ";", 1, loc);
    case '?':
        return match(lex, '.') ? build_token(lex, TOKEN_QUESTION_DOT, "?.", 2, loc)
                               : build_token(lex, TOKEN_QUESTION, "?", 1, loc);
    case '=':
    case '!':
    case '<':
    case '>':
    case '&':
    case '|':
        return scan_comparison_or_logical(lex, c, loc);
    case '+':
    case '-':
    case '*':
    case '/':
    case '%':
        return scan_arithmetic(lex, c, loc);
    default:
        break;
    }
    rsg_err(loc, "unexpected character '%c'", c);
    return build_token(lex, TOKEN_ERR, &lex->src[lex->pos - 1], 1, loc);
}

/**
 * Core scanner - skips whitespace and comments, then dispatches to the
 * appropriate scan_* routine.  Used by both lex_next and interpolation
 * scanning.
 */
Token scan_token(Lex *lex) {
    // Skip horizontal whitespace and comments.
    for (;;) {
        while (peek(lex) == ' ' || peek(lex) == '\t' || peek(lex) == '\r') {
            advance(lex);
        }

        // Newlines are significant - emit TOKEN_NEWLINE as a stmt
        // terminator.
        if (peek(lex) == '\n') {
            SrcLoc loc = current_loc(lex);
            advance(lex); // consume '\n'
            lex->line++;
            lex->column = 1;
            return build_token(lex, TOKEN_NEWLINE, "\n", 1, loc);
        }

        // Line comments: skip until end of line.
        if (peek(lex) == '/' && peek_next(lex) == '/') {
            while (peek(lex) != '\n' && peek(lex) != '\0') {
                advance(lex);
            }
            continue;
        }

        break;
    }

    SrcLoc loc = current_loc(lex);
    char c = advance(lex);

    if (c == '\0') {
        return build_token(lex, TOKEN_EOF, "", 0, loc);
    }
    if (is_digit(c)) {
        return scan_number(lex, loc);
    }
    if (is_alpha(c)) {
        return scan_ident(lex, loc);
    }
    if (c == '"') {
        return scan_str(lex, loc);
    }
    if (c == '\'') {
        return scan_char_lit(lex, loc);
    }
    return scan_punctuation(lex, c, loc);
}

Lex *lex_create(const char *src, const char *file, Arena *arena) {
    Lex *lex = rsg_malloc(sizeof(*lex));
    lex->src = src;
    lex->file = file;
    lex->pos = 0;
    lex->len = (int32_t)strlen(src);
    lex->line = 1;
    lex->column = 1;
    lex->arena = arena;
    lex->pending = NULL;
    lex->pending_pos = 0;
    lex->last_kind = TOKEN_EOF;
    return lex;
}

void lex_destroy(Lex *lex) {
    if (lex != NULL) {
        BUF_FREE(lex->pending);
        free(lex);
    }
}

Token lex_next(Lex *lex) {
    // Return pending tokens first (from str interpolation)
    if (lex->pending != NULL && lex->pending_pos < BUF_LEN(lex->pending)) {
        Token token = lex->pending[lex->pending_pos++];
        lex->last_kind = token.kind;
        return token;
    }
    // Free pending buf when exhausted
    if (lex->pending != NULL) {
        BUF_FREE(lex->pending);
        lex->pending = NULL;
        lex->pending_pos = 0;
    }
    Token token = scan_token(lex);
    lex->last_kind = token.kind;
    return token;
}

Token *lex_scan_all(Lex *lex) {
    Token *tokens = NULL;
    for (;;) {
        Token token = lex_next(lex);
        BUF_PUSH(tokens, token);
        if (token.kind == TOKEN_EOF) {
            break;
        }
    }
    return tokens;
}
