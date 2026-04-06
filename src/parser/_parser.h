#ifndef RG__PARSER_H
#define RG__PARSER_H

#include "rsg/parser.h"

/**
 * @file _parser.h
 * @brief Internal declarations shared across parser translation units.
 *
 * Not part of the public API -- only included by lib/frontend/parser/ files.
 */

// ── Struct definition ──────────────────────────────────────────────────

struct Parser {
    const Token *tokens; /* buf */
    int32_t position;
    int32_t count;
    int32_t error_count;
    Arena *arena;     // AST allocation arena
    const char *file; // source filename for diagnostics
};

// ── Token-stream navigation (parser/helpers.c) ────────────────────────

/** Return the token at the current position. */
const Token *parser_current_token(const Parser *parser);
/** Return the token just before the current position. */
const Token *parser_previous_token(const Parser *parser);
/** True when the current token is TOKEN_EOF. */
bool parser_at_end(const Parser *parser);
/** True when the current token matches @p kind. */
bool parser_check(const Parser *parser, TokenKind kind);
/** Advance past the current token and return the previous one. */
const Token *parser_advance(Parser *parser);
/** If the current token matches @p kind, consume it and return true. */
bool parser_match(Parser *parser, TokenKind kind);
/**
 * Consume the current token if it matches @p kind; otherwise emit an error
 * and return the current token without advancing.
 */
const Token *parser_expect(Parser *parser, TokenKind kind);
/** Skip consecutive newline tokens. */
void parser_skip_newlines(Parser *parser);
/** Return the source location of the current token. */
SourceLocation parser_current_location(const Parser *parser);

// ── Type parsing (parser/types.c) ──────────────────────────────────────

/** Parse an explicit type annotation (e.g. i32, str, bool, [N]T, (A,B)). */
ASTType parser_parse_type(Parser *parser);

// ── Expression parsing (parser/expression.c) ───────────────────────────

/** Expression dispatch — returns a parsed expression AST node. */
ASTNode *parser_parse_expression(Parser *parser);

// ── Statement / block parsing (parser/statement.c) ─────────────────────

/** Parse a brace-enclosed block: { stmts... [trailing_expr] }. */
ASTNode *parser_parse_block(Parser *parser);
/** Parse a single statement inside a block. */
ASTNode *parser_parse_statement(Parser *parser);

// ── Declaration parsing (parser/declaration.c) ─────────────────────────

/** Parse a top-level declaration (module, type, fn, pub fn, or statement). */
ASTNode *parser_parse_declaration(Parser *parser);

#endif // RG__PARSER_H
