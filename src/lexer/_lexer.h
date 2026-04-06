#ifndef RG__LEXER_H
#define RG__LEXER_H

#include "lexer.h"

/**
 * @file _lexer.h
 * @brief Internal decls shared across lexer translation units.
 *
 * Not part of the pub API -- only included by src/lexer/ files.
 */

// ── Struct def ──────────────────────────────────────────────────

struct Lexer {
    const char *source;
    const char *file; // source filename for diagnostics
    int32_t pos;
    int32_t len;
    int32_t line;             // 1-based current line
    int32_t column;           // 1-based current column
    Arena *arena;             // for allocating lexemes / str lits
    Token *pending; /* buf */ // bufed tokens from str interpolation
    int32_t pending_pos;      // read cursor into pending
    TokenKind last_kind;      // previous token kind (for ctx-sensitive scanning)
};

// ── Character-level helpers ────────────────────────────────────────────

static inline char peek(const Lexer *lexer) {
    return (char)(lexer->pos < lexer->len ? lexer->source[lexer->pos] : '\0');
}

static inline char peek_next(const Lexer *lexer) {
    return (char)(lexer->pos + 1 < lexer->len ? lexer->source[lexer->pos + 1] : '\0');
}

static inline char advance(Lexer *lexer) {
    char c = peek(lexer);
    lexer->pos++;
    lexer->column++;
    return c;
}

static inline bool match(Lexer *lexer, char expected) {
    if (peek(lexer) == expected) {
        advance(lexer);
        return true;
    }
    return false;
}

static inline SourceLoc current_loc(const Lexer *lexer) {
    return (SourceLoc){.file = lexer->file, .line = lexer->line, .column = lexer->column};
}

// ── Token construction ─────────────────────────────────────────────────

Token make_token(const Lexer *lexer, TokenKind kind, const char *start, int32_t len, SourceLoc loc);

// ── Cross-file dispatch ────────────────────────────────────────────────

/** Core scanner: skip whitespace/comments, then dispatch to scanning routines. */
Token scan_token(Lexer *lexer);
/** Scan a str lit (simple or interpolated). */
Token scan_str(Lexer *lexer, SourceLoc loc);
/** Scan a character lit: 'A', '\n', '\t', '\\', '\'', '\0'. */
Token scan_char_lit(Lexer *lexer, SourceLoc loc);

#endif // RG__LEXER_H
