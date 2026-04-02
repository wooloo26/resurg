#ifndef RG_TOKEN_H
#define RG_TOKEN_H

#include "common.h"

// ---------------------------------------------------------------------------
// Token kinds — every lexeme the v0.1.0 lexer can produce.
// ---------------------------------------------------------------------------
typedef enum {
    // Literals
    TOK_INT_LIT,   // 42, 1_000
    TOK_FLOAT_LIT, // 3.14, 2.5e10
    TOK_STR_LIT,   // "hello"
    TOK_TRUE,      // true
    TOK_FALSE,     // false

    // Identifiers
    TOK_IDENT, // foo, bar

    // Keywords
    TOK_MODULE,   // module
    TOK_PUB,      // pub
    TOK_FN,       // fn
    TOK_VAR,      // var
    TOK_IF,       // if
    TOK_ELSE,     // else
    TOK_LOOP,     // loop
    TOK_FOR,      // for
    TOK_BREAK,    // break
    TOK_CONTINUE, // continue
    TOK_ASSERT,   // assert

    // Type keywords (v0.1.0 subset)
    TOK_BOOL, // bool
    TOK_I32,  // i32
    TOK_U32,  // u32
    TOK_F64,  // f64
    TOK_STR,  // str
    TOK_UNIT, // unit

    // Operators — arithmetic
    TOK_PLUS,    // +
    TOK_MINUS,   // -
    TOK_STAR,    // *
    TOK_SLASH,   // /
    TOK_PERCENT, // %

    // Operators — comparison
    TOK_EQ_EQ,   // ==
    TOK_BANG_EQ, // !=
    TOK_LT,      // <
    TOK_LT_EQ,   // <=
    TOK_GT,      // >
    TOK_GT_EQ,   // >=

    // Operators — logical
    TOK_AMP_AMP,   // &&
    TOK_PIPE_PIPE, // ||
    TOK_BANG,      // !
    TOK_PIPE,      // |

    // Operators — assignment
    TOK_COLON_EQ, // :=
    TOK_EQ,       // =
    TOK_PLUS_EQ,  // +=
    TOK_MINUS_EQ, // -=
    TOK_STAR_EQ,  // *=
    TOK_SLASH_EQ, // /=

    // Punctuation
    TOK_LPAREN,    // (
    TOK_RPAREN,    // )
    TOK_LBRACE,    // {
    TOK_RBRACE,    // }
    TOK_COLON,     // :
    TOK_COMMA,     // ,
    TOK_DOT_DOT,   // ..
    TOK_DOT,       // .
    TOK_ARROW,     // ->
    TOK_SEMICOLON, // ; (optional, for future use)

    // String interpolation (inside strings)
    TOK_INTERP_START, // start of interpolation segment
    TOK_INTERP_END,   // end of interpolation segment

    // Special
    TOK_NEWLINE, // significant newline (statement terminator)
    TOK_EOF,     // end of file
    TOK_ERROR,   // lexer error
} TokenKind;

// ---------------------------------------------------------------------------
// Token — a single lexeme with source location.
// ---------------------------------------------------------------------------
typedef struct {
    TokenKind kind;
    const char *lexeme; // points into arena-duped source or interned string
    int32_t length;     // length of lexeme
    SrcLoc loc;

    // Literal values (populated for literal tokens)
    union {
        int64_t int_val;  // TOK_INT_LIT
        double float_val; // TOK_FLOAT_LIT
        char *str_val;    // TOK_STR_LIT (unescaped, arena-allocated)
    } lit;
} Token;

// Return a human-readable name for a token kind.
const char *token_kind_str(TokenKind kind);

#endif // RG_TOKEN_H
