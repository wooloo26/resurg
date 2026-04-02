#ifndef RG_PARSER_H
#define RG_PARSER_H

#include "ast.h"

// ------------------------------------------------------------------------
// Parser — recursive descent, transforms tokens into AST.
// ------------------------------------------------------------------------
typedef struct Parser Parser;

// Create a parser for the given token stream.
Parser *parser_create(const Token *tokens, int32_t count, Arena *arena, const char *file);
// Destroy the parser.
void parser_destroy(Parser *p);
// Parse the token stream into a NODE_FILE AST.
ASTNode *parser_parse(Parser *p);

#endif // RG_PARSER_H
