#ifndef RSG__PARSE_H
#define RSG__PARSE_H

#include "core/diag.h"
#include "rsg/pass/parse/parse.h"

/**
 * @file _parse.h
 * @brief Internal decls shared across parser translation units.
 *
 * Not part of the pub API -- only included by src/pass/parse/ files.
 */

// ── Struct def ──────────────────────────────────────────────────

struct Parser {
    const Token *tokens; /* buf */
    int32_t pos;
    int32_t count;
    int32_t err_count;
    Arena *arena;            // AST alloc arena
    DiagCtx *dctx;           // structured diagnostic collector (NULL → stderr fallback)
    const char *file;        // src filename for diagnostics
    bool no_struct_lit;      // suppress struct-lit parsing in if/while conditions
    const char *pending_doc; // accumulated doc comment text for next decl (arena-owned)
};

/** Report a parse err through the structured diagnostic context. */
#define PARSER_ERR(parser, loc, ...)                                                               \
    do {                                                                                           \
        if ((parser)->dctx != NULL) {                                                              \
            diag_at((parser)->dctx, DIAG_ERR, loc, NULL, __VA_ARGS__);                             \
        } else {                                                                                   \
            rsg_err(loc, __VA_ARGS__);                                                             \
        }                                                                                          \
        (parser)->err_count++;                                                                     \
    } while (0)

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
/** Skip newlines/semicolons only, NOT doc-comment tokens. */
void parser_skip_lines_only(Parser *parser);
/** Skip newlines and collect doc comments into parser->pending_doc. */
void parser_skip_newlines_collect_docs(Parser *parser);
/** Consume and return the pending doc comment, resetting it to NULL. */
const char *parser_take_doc(Parser *parser);
/** Return the src loc of the current token. */
SrcLoc parser_current_loc(const Parser *parser);
/** Peek one token ahead without consuming. */
bool parser_peek_is(const Parser *parser, TokenKind kind);

// ── Type parsing (parser/types.c) ──────────────────────────────────────

/** Parse an explicit type annotation (e.g. i32, str, bool, [N]T, (A,B)). */
ASTType parser_parse_type(Parser *parser);

// ── Expression parsing (parser/expr.c) ───────────────────────────

/** Expression dispatch — returns a parsed expr AST node. */
ASTNode *parser_parse_expr(Parser *parser);

// ── Lookahead helpers (parser/helpers.c) ────────────────────────────

/** Return true when the token stream starting at the current position
 *  looks like a pattern-binding form: `Pattern := expr`. */
bool parser_is_pattern_binding(const Parser *parser);

// ── Match / pattern parsing (parser/match.c) ──────────────────────

/** Parse a match expression: match operand { pattern => body, ... }. */
ASTNode *parser_parse_match(Parser *parser);
/** Parse a single pattern (for if-var/while-var/match arms). */
ASTPattern *parser_parse_pattern(Parser *parser);

// ── Statement / block parsing (parser/stmt.c) ─────────────────────

/** Parse a brace-enclosed block: { stmts... [trailing_expr] }. */
ASTNode *parser_parse_block(Parser *parser);
/** Parse a single stmt inside a block. */
ASTNode *parser_parse_stmt(Parser *parser);

// ── Declaration parsing (parser/decl.c) ─────────────────────────

/** Parse a top-level decl (module, type, fn, pub fn, or stmt). */
ASTNode *parser_parse_decl(Parser *parser);

// ── Fn / method parsing (parser/parse_fn.c) ─────────────────────

/** Parse generic type param list: `<T, U: Bound + Bound2, ...>`. */
ASTTypeParam *parse_type_params(Parser *parser);
/** Parse where clauses: `where T: Bound1 + Bound2, U: Bound3`. */
ASTWhereClause *parse_where_clauses(Parser *parser);
/** Parse method receiver and params between `(` and `)`. */
void parse_recv_and_params(Parser *parser, ASTNode *node);
/** Parse a method decl inside a struct/enum/ext block. */
ASTNode *parse_method_decl(Parser *parser, const char *struct_name, bool is_pub);
/** Parse a top-level fn decl. */
ASTNode *parse_fn_decl(Parser *parser, bool is_pub);
/** Parse `decl var name: Type`. */
ASTNode *parse_declare_var(Parser *parser, bool is_pub);

#endif // RSG__PARSE_H
