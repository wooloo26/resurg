#ifndef RG__LEXER_H
#define RG__LEXER_H

#include "lexer.h"

/**
 * @file _lexer.h
 * @brief Internal declarations shared across lexer translation units.
 *
 * Not part of the public API -- only included by src/lexer/ files.
 */

// ── Struct definition ──────────────────────────────────────────────────

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
    TokenKind last_kind;      // previous token kind (for context-sensitive scanning)
};

// ── Character-level helpers ────────────────────────────────────────────

static inline char peek(const Lexer *lexer) {
    return (char)(lexer->position < lexer->length ? lexer->source[lexer->position] : '\0');
}

static inline char peek_next(const Lexer *lexer) {
    return (char)(lexer->position + 1 < lexer->length ? lexer->source[lexer->position + 1] : '\0');
}

static inline char advance(Lexer *lexer) {
    char c = peek(lexer);
    lexer->position++;
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

static inline SourceLocation current_location(const Lexer *lexer) {
    return (SourceLocation){.file = lexer->file, .line = lexer->line, .column = lexer->column};
}

// ── Token construction ─────────────────────────────────────────────────

Token make_token(const Lexer *lexer, TokenKind kind, const char *start, int32_t length,
                 SourceLocation location);

// ── Cross-file dispatch ────────────────────────────────────────────────

/** Core scanner: skip whitespace/comments, then dispatch to scanning routines. */
Token scan_token(Lexer *lexer);
/** Scan a string literal (simple or interpolated). */
Token scan_string(Lexer *lexer, SourceLocation location);
/** Scan a character literal: 'A', '\n', '\t', '\\', '\'', '\0'. */
Token scan_char_literal(Lexer *lexer, SourceLocation location);

#endif // RG__LEXER_H
