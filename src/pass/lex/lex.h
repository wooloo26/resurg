#ifndef RSG_LEX_H
#define RSG_LEX_H

#include "core/token.h"

/**
 * @file lex.h
 * @brief Lex - transforms src text into a flat stream of Tokens.
 *
 * Handles str interpolation by expanding interpolated strs into a
 * sequence of STR_LIT / INTERPOLATION_START / ... / INTERPOLATION_END
 * tokens.
 */
typedef struct Lex Lex;
typedef struct DiagCtx DiagCtx;

/**
 * Create a lex over @p src (owned by caller).  Tokens and lexemes are
 * allocated from @p arena.
 */
/**
 * Create a lex over @p src (owned by caller).  Tokens and lexemes are
 * allocated from @p arena.  If @p dctx is non-NULL, lex errs are
 * collected via the diagnostic context; otherwise they go to stderr.
 */
Lex *lex_create(const char *src, const char *file, Arena *arena, DiagCtx *dctx);
/** Destroy the lex and free its internal bookkeeping. */
void lex_destroy(Lex *lex);
/** Return the next token (may drain the interpolation pending buf first). */
Token lex_next(Lex *lex);
/** Lex the entire src and return a stretchy buf of Tokens, terminated by TOKEN_EOF. */
Token *lex_scan_all(Lex *lex);

#endif // RSG_LEX_H
