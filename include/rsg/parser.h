#ifndef RG_PARSER_H
#define RG_PARSER_H

#include "ast/ast.h"

/**
 * @file parser.h
 * @brief Recursive-descent parser - transforms a token stream into an AST.
 */
typedef struct Parser Parser;

/**
 * Create a parser over @p count tokens.  AST nodes are allocated from
 * @p arena.
 */
Parser *parser_create(const Token *tokens, int32_t count, Arena *arena, const char *file);
/** Destroy the parser (does not free the arena). */
void parser_destroy(Parser *parser);
/** Parse the full token stream and return the root NODE_FILE. */
ASTNode *parser_parse(Parser *parser);
/** Return the number of errs accumulated during parsing. */
int32_t parser_err_count(const Parser *parser);

#endif // RG_PARSER_H
