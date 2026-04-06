#ifndef RG_LEXER_H
#define RG_LEXER_H

#include "core/token.h"

/**
 * @file lexer.h
 * @brief Lexer - transforms source text into a flat stream of Tokens.
 *
 * Handles str interpolation by expanding interpolated strs into a
 * sequence of STR_LIT / INTERPOLATION_START / ... / INTERPOLATION_END
 * tokens.
 */
typedef struct Lexer Lexer;

/**
 * Create a lexer over @p source (owned by caller).  Tokens and lexemes are
 * allocated from @p arena.
 */
Lexer *lexer_create(const char *source, const char *file, Arena *arena);
/** Destroy the lexer and free its internal bookkeeping. */
void lexer_destroy(Lexer *lexer);
/** Return the next token (may drain the interpolation pending buf first). */
Token lexer_next(Lexer *lexer);
/** Lex the entire source and return a stretchy buf of Tokens, terminated by TOKEN_EOF. */
Token *lexer_scan_all(Lexer *lexer);

#endif // RG_LEXER_H
