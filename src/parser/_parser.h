#ifndef RG__PARSER_H
#define RG__PARSER_H

#include "rsg/parser.h"

/**
 * @file _parser.h
 * @brief Internal decls shared across parser translation units.
 *
 * Not part of the pub API -- only included by lib/frontend/parser/ files.
 */

// ── Struct def ──────────────────────────────────────────────────

struct Parser {
    const Token *tokens; /* buf */
    int32_t pos;
    int32_t count;
    int32_t err_count;
    Arena *arena;     // AST alloc arena
    const char *file; // source filename for diagnostics
};

// ── Token-stream navigation (parser/helpers.c) ────────────────────────

/** Return the token at the current pos. */
const Token *parser_current_token(const Parser *parser);
/** Return the token just before the current pos. */
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
 * Consume the current token if it matches @p kind; otherwise emit an err
 * and return the current token without advancing.
 */
const Token *parser_expect(Parser *parser, TokenKind kind);
/** Skip consecutive newline tokens. */
void parser_skip_newlines(Parser *parser);
/** Return the source loc of the current token. */
SourceLoc parser_current_loc(const Parser *parser);

// ── Type parsing (parser/types.c) ──────────────────────────────────────

/** Parse an explicit type annotation (e.g. i32, str, bool, [N]T, (A,B)). */
ASTType parser_parse_type(Parser *parser);

// ── Expression parsing (parser/expr.c) ───────────────────────────

/** Expression dispatch — returns a parsed expr AST node. */
ASTNode *parser_parse_expr(Parser *parser);

// ── Statement / block parsing (parser/stmt.c) ─────────────────────

/** Parse a brace-enclosed block: { stmts... [trailing_expr] }. */
ASTNode *parser_parse_block(Parser *parser);
/** Parse a single stmt inside a block. */
ASTNode *parser_parse_stmt(Parser *parser);

// ── Declaration parsing (parser/decl.c) ─────────────────────────

/** Parse a top-level decl (module, type, fn, pub fn, or stmt). */
ASTNode *parser_parse_decl(Parser *parser);

#endif // RG__PARSER_H
