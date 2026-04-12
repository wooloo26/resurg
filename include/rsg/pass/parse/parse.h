#ifndef RSG_PARSE_H
#define RSG_PARSE_H

#include "repr/ast.h"

/**
 * @file parse.h
 * @brief Recursive-descent parser - transforms a token stream into an AST.
 */
typedef struct Parser Parser;
typedef struct DiagCtx DiagCtx;

/**
 * Create a parser over @p count tokens.  AST nodes are allocated from
 * @p arena.  If @p dctx is non-NULL, parse errs are collected via the
 * diagnostic context; otherwise they go to stderr.
 */
Parser *parser_create(const Token *tokens, int32_t count, Arena *arena, const char *file,
                      DiagCtx *dctx);
/** Destroy the parser (does not free the arena). */
void parser_destroy(Parser *parser);
/** Parse the full token stream and return the root NODE_FILE. */
ASTNode *parser_parse(Parser *parser);
/** Return the number of errs accumulated during parsing. */
int32_t parser_err_count(const Parser *parser);

#endif // RSG_PARSE_H
