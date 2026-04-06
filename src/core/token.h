#ifndef RSG_TOKEN_H
#define RSG_TOKEN_H

#include "common.h"

/**
 * @file token.h
 * @brief Token kinds and the Token struct produced by the lex.
 */

/** Every lexeme the lex can produce. */
typedef enum {
    // Lits
    TOKEN_INTEGER_LIT, // 42, 1_000
    TOKEN_FLOAT_LIT,   // 3.14, 2.5e10
    TOKEN_STR_LIT,     // "hello"
    TOKEN_CHAR_LIT,    // 'A', '\n'
    TOKEN_TRUE,        // true
    TOKEN_FALSE,       // false

    // Identifiers
    TOKEN_ID, // foo, bar

    // Keywords
    TOKEN_MODULE,   // module
    TOKEN_PUB,      // pub
    TOKEN_FN,       // fn
    TOKEN_VAR,      // var
    TOKEN_IF,       // if
    TOKEN_ELSE,     // else
    TOKEN_LOOP,     // loop
    TOKEN_FOR,      // for
    TOKEN_BREAK,    // break
    TOKEN_CONTINUE, // continue
    TOKEN_TYPE,     // type
    TOKEN_STRUCT,   // struct
    TOKEN_MUT,      // mut
    TOKEN_IMMUT,    // immut
    TOKEN_ENUM,     // enum
    TOKEN_PACT,     // pact
    TOKEN_MATCH,    // match
    TOKEN_RETURN,   // return
    TOKEN_WHILE,    // while
    TOKEN_DEFER,    // defer

    // Type keywords
    TOKEN_BOOL,  // bool
    TOKEN_I8,    // i8
    TOKEN_I16,   // i16
    TOKEN_I32,   // i32
    TOKEN_I64,   // i64
    TOKEN_I128,  // i128
    TOKEN_U8,    // u8
    TOKEN_U16,   // u16
    TOKEN_U32,   // u32
    TOKEN_U64,   // u64
    TOKEN_U128,  // u128
    TOKEN_ISIZE, // isize
    TOKEN_USIZE, // usize
    TOKEN_F32,   // f32
    TOKEN_F64,   // f64
    TOKEN_CHAR,  // char
    TOKEN_STR,   // str
    TOKEN_UNIT,  // unit
    TOKEN_NEVER, // never

    // Operators - arithmetic
    TOKEN_PLUS,    // +
    TOKEN_MINUS,   // -
    TOKEN_STAR,    // *
    TOKEN_SLASH,   // /
    TOKEN_PERCENT, // %

    // Operators - comparison
    TOKEN_EQUAL_EQUAL,   // ==
    TOKEN_BANG_EQUAL,    // !=
    TOKEN_LESS,          // <
    TOKEN_LESS_EQUAL,    // <=
    TOKEN_GREATER,       // >
    TOKEN_GREATER_EQUAL, // >=

    // Operators - logical
    TOKEN_AMPERSAND,           // &
    TOKEN_AMPERSAND_AMPERSAND, // &&
    TOKEN_PIPE_PIPE,           // ||
    TOKEN_BANG,                // !
    TOKEN_PIPE,                // |

    // Operators - assignment
    TOKEN_COLON_EQUAL, // :=
    TOKEN_EQUAL,       // =
    TOKEN_PLUS_EQUAL,  // +=
    TOKEN_MINUS_EQUAL, // -=
    TOKEN_STAR_EQUAL,  // *=
    TOKEN_SLASH_EQUAL, // /=

    // Punctuation
    TOKEN_LEFT_PAREN,    // (
    TOKEN_RIGHT_PAREN,   // )
    TOKEN_LEFT_BRACE,    // {
    TOKEN_RIGHT_BRACE,   // }
    TOKEN_LEFT_BRACKET,  // [
    TOKEN_RIGHT_BRACKET, // ]
    TOKEN_COLON_COLON,   // ::
    TOKEN_COLON,         // :
    TOKEN_COMMA,         // ,
    TOKEN_DOT_DOT_EQUAL, // ..=
    TOKEN_DOT_DOT,       // ..
    TOKEN_DOT,           // .
    TOKEN_FAT_ARROW,     // =>
    TOKEN_ARROW,         // ->
    TOKEN_SEMICOLON,     // ; (optional, for future use)

    // Str interpolation (inside strs)
    TOKEN_INTERPOLATION_START, // start of interpolation segment
    TOKEN_INTERPOLATION_END,   // end of interpolation segment

    // Special
    TOKEN_NEWLINE, // significant newline (stmt terminator)
    TOKEN_EOF,     // end of file
    TOKEN_ERR,     // lex err
} TokenKind;

/**
 * A single lexeme with its kind, src text, loc, and optional
 * parsed lit value.
 */
typedef struct {
    TokenKind kind;
    const char *lexeme; // points into arena-duped src or interned str
    int32_t len;
    SrcLoc loc;

    /** Parsed lit payload (valid only for lit token kinds). */
    union {
        uint64_t integer_value;
        double float_value;
        char *str_value;     // unescaped, arena-allocated
        uint32_t char_value; // Unicode scalar value (for TOKEN_CHAR_LIT)
    } lit_value;
} Token;

/** Return a human-readable name for @p kind (e.g. "ID", "+"). */
const char *token_kind_str(TokenKind kind);

/** Return true if @p kind is a type keyword (bool, i8, ..., str, unit). */
bool token_is_type_keyword(TokenKind kind);

#endif // RSG_TOKEN_H
