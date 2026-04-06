#ifndef RSG__LEX_H
#define RSG__LEX_H

#include "lex.h"

/**
 * @file _lex.h
 * @brief Internal decls shared across lex translation units.
 *
 * Not part of the pub API -- only included by src/lex/ files.
 */

// ── Struct def ──────────────────────────────────────────────────

struct Lex {
    const char *src;
    const char *file; // src filename for diagnostics
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

static inline char peek(const Lex *lex) {
    return (char)(lex->pos < lex->len ? lex->src[lex->pos] : '\0');
}

static inline char peek_next(const Lex *lex) {
    return (char)(lex->pos + 1 < lex->len ? lex->src[lex->pos + 1] : '\0');
}

static inline char advance(Lex *lex) {
    char c = peek(lex);
    lex->pos++;
    lex->column++;
    return c;
}

static inline bool match(Lex *lex, char expected) {
    if (peek(lex) == expected) {
        advance(lex);
        return true;
    }
    return false;
}

static inline SrcLoc current_loc(const Lex *lex) {
    return (SrcLoc){.file = lex->file, .line = lex->line, .column = lex->column};
}

// ── Token construction ─────────────────────────────────────────────────

Token build_token(const Lex *lex, TokenKind kind, const char *start, int32_t len, SrcLoc loc);

// ── Cross-file dispatch ────────────────────────────────────────────────

/** Core scanner: skip whitespace/comments, then dispatch to scanning routines. */
Token scan_token(Lex *lex);
/** Scan a str lit (simple or interpolated). */
Token scan_str(Lex *lex, SrcLoc loc);
/** Scan a character lit: 'A', '\n', '\t', '\\', '\'', '\0'. */
Token scan_char_lit(Lex *lex, SrcLoc loc);

#endif // RSG__LEX_H
