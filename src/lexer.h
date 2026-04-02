#ifndef RG_LEXER_H
#define RG_LEXER_H

#include "token.h"

// ---------------------------------------------------------------------------
// Lexer — transforms source text into a stream of tokens.
// ---------------------------------------------------------------------------
typedef struct Lexer Lexer;

// Create a lexer for the given source text.
Lexer *lexer_create(const char *source, const char *file, Arena *arena);
// Destroy the lexer and free internal buffers.
void lexer_destroy(Lexer *l);
// Return the next token from the source.
Token lexer_next(Lexer *l);
// Scan all tokens into a stretchy buffer.
Token *lexer_scan_all(Lexer *l);

#endif // RG_LEXER_H
