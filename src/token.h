#ifndef RG_TOKEN_H
#define RG_TOKEN_H

#include "common.h"

/**
 * @file token.h
 * @brief Token kinds and the Token struct produced by the lexer.
 */

/** Every lexeme the lexer can produce. */
typedef enum {
    // Literals
    TOKEN_INTEGER_LITERAL, // 42, 1_000
    TOKEN_FLOAT_LITERAL,   // 3.14, 2.5e10
    TOKEN_STRING_LITERAL,  // "hello"
    TOKEN_CHAR_LITERAL,    // 'A', '\n'
    TOKEN_TRUE,            // true
    TOKEN_FALSE,           // false

    // Identifiers
    TOKEN_IDENTIFIER, // foo, bar

    // Keywords
    TOKEN_MODULE,   // module
    TOKEN_PUBLIC,   // pub
    TOKEN_FUNCTION, // fn
    TOKEN_VARIABLE, // var
    TOKEN_IF,       // if
    TOKEN_ELSE,     // else
    TOKEN_LOOP,     // loop
    TOKEN_FOR,      // for
    TOKEN_BREAK,    // break
    TOKEN_CONTINUE, // continue
    TOKEN_TYPE,     // type

    // Type keywords
    TOKEN_BOOL,   // bool
    TOKEN_I8,     // i8
    TOKEN_I16,    // i16
    TOKEN_I32,    // i32
    TOKEN_I64,    // i64
    TOKEN_I128,   // i128
    TOKEN_U8,     // u8
    TOKEN_U16,    // u16
    TOKEN_U32,    // u32
    TOKEN_U64,    // u64
    TOKEN_U128,   // u128
    TOKEN_ISIZE,  // isize
    TOKEN_USIZE,  // usize
    TOKEN_F32,    // f32
    TOKEN_F64,    // f64
    TOKEN_CHAR,   // char
    TOKEN_STRING, // str
    TOKEN_UNIT,   // unit

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
    TOKEN_COLON,         // :
    TOKEN_COMMA,         // ,
    TOKEN_DOT_DOT,       // ..
    TOKEN_DOT,           // .
    TOKEN_ARROW,         // ->
    TOKEN_SEMICOLON,     // ; (optional, for future use)

    // String interpolation (inside strings)
    TOKEN_INTERPOLATION_START, // start of interpolation segment
    TOKEN_INTERPOLATION_END,   // end of interpolation segment

    // Special
    TOKEN_NEWLINE, // significant newline (statement terminator)
    TOKEN_EOF,     // end of file
    TOKEN_ERROR,   // lexer error
} TokenKind;

/**
 * A single lexeme with its kind, source text, location, and optional
 * parsed literal value.
 */
typedef struct {
    TokenKind kind;
    const char *lexeme; // points into arena-duped source or interned string
    int32_t length;
    SourceLocation location;

    /** Parsed literal payload (valid only for literal token kinds). */
    union {
        uint64_t integer_value;
        double float_value;
        char *string_value; // unescaped, arena-allocated
        char char_value;    // for TOKEN_CHAR_LITERAL
    } literal_value;
} Token;

/** Return a human-readable name for @p kind (e.g. "IDENTIFIER", "+"). */
const char *token_kind_string(TokenKind kind);

/** Return true if @p kind is a type keyword (bool, i8, ..., str, unit). */
bool token_is_type_keyword(TokenKind kind);

#endif // RG_TOKEN_H
