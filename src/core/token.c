#include "token.h"

/** Map TokenKind -> display string.  Used by diagnostics and --dump-tokens. */
const char *token_kind_string(TokenKind kind) {
    static const char *names[] = {
        [TOKEN_INTEGER_LITERAL] = "INTEGER_LITERAL",
        [TOKEN_FLOAT_LITERAL] = "FLOAT_LITERAL",
        [TOKEN_STRING_LITERAL] = "STRING_LITERAL",
        [TOKEN_CHAR_LITERAL] = "CHAR_LITERAL",
        [TOKEN_TRUE] = "true",
        [TOKEN_FALSE] = "false",
        [TOKEN_IDENTIFIER] = "IDENTIFIER",
        [TOKEN_MODULE] = "module",
        [TOKEN_PUBLIC] = "pub",
        [TOKEN_FUNCTION] = "fn",
        [TOKEN_VARIABLE] = "var",
        [TOKEN_IF] = "if",
        [TOKEN_ELSE] = "else",
        [TOKEN_LOOP] = "loop",
        [TOKEN_FOR] = "for",
        [TOKEN_BREAK] = "break",
        [TOKEN_CONTINUE] = "continue",
        [TOKEN_TYPE] = "type",
        [TOKEN_STRUCT] = "struct",
        [TOKEN_MUT] = "mut",
        [TOKEN_BOOL] = "bool",
        [TOKEN_I8] = "i8",
        [TOKEN_I16] = "i16",
        [TOKEN_I32] = "i32",
        [TOKEN_I64] = "i64",
        [TOKEN_I128] = "i128",
        [TOKEN_U8] = "u8",
        [TOKEN_U16] = "u16",
        [TOKEN_U32] = "u32",
        [TOKEN_U64] = "u64",
        [TOKEN_U128] = "u128",
        [TOKEN_ISIZE] = "isize",
        [TOKEN_USIZE] = "usize",
        [TOKEN_F32] = "f32",
        [TOKEN_F64] = "f64",
        [TOKEN_CHAR] = "char",
        [TOKEN_STRING] = "str",
        [TOKEN_UNIT] = "unit",
        [TOKEN_PLUS] = "+",
        [TOKEN_MINUS] = "-",
        [TOKEN_STAR] = "*",
        [TOKEN_SLASH] = "/",
        [TOKEN_PERCENT] = "%",
        [TOKEN_EQUAL_EQUAL] = "==",
        [TOKEN_BANG_EQUAL] = "!=",
        [TOKEN_LESS] = "<",
        [TOKEN_LESS_EQUAL] = "<=",
        [TOKEN_GREATER] = ">",
        [TOKEN_GREATER_EQUAL] = ">=",
        [TOKEN_AMPERSAND_AMPERSAND] = "&&",
        [TOKEN_PIPE_PIPE] = "||",
        [TOKEN_BANG] = "!",
        [TOKEN_PIPE] = "|",
        [TOKEN_COLON_EQUAL] = ":=",
        [TOKEN_EQUAL] = "=",
        [TOKEN_PLUS_EQUAL] = "+=",
        [TOKEN_MINUS_EQUAL] = "-=",
        [TOKEN_STAR_EQUAL] = "*=",
        [TOKEN_SLASH_EQUAL] = "/=",
        [TOKEN_LEFT_PAREN] = "(",
        [TOKEN_RIGHT_PAREN] = ")",
        [TOKEN_LEFT_BRACE] = "{",
        [TOKEN_RIGHT_BRACE] = "}",
        [TOKEN_LEFT_BRACKET] = "[",
        [TOKEN_RIGHT_BRACKET] = "]",
        [TOKEN_COLON] = ":",
        [TOKEN_COMMA] = ",",
        [TOKEN_DOT_DOT] = "..",
        [TOKEN_DOT] = ".",
        [TOKEN_ARROW] = "->",
        [TOKEN_SEMICOLON] = ";",
        [TOKEN_INTERPOLATION_START] = "INTERPOLATION_START",
        [TOKEN_INTERPOLATION_END] = "INTERPOLATION_END",
        [TOKEN_NEWLINE] = "NEWLINE",
        [TOKEN_EOF] = "EOF",
        [TOKEN_ERROR] = "ERROR",
    };
    if (kind >= 0 && kind < (int32_t)(sizeof(names) / sizeof(names[0])) && names[kind] != NULL) {
        return names[kind];
    }
    return "?";
}

bool token_is_type_keyword(TokenKind kind) {
    switch (kind) {
    case TOKEN_BOOL:
    case TOKEN_I8:
    case TOKEN_I16:
    case TOKEN_I32:
    case TOKEN_I64:
    case TOKEN_I128:
    case TOKEN_U8:
    case TOKEN_U16:
    case TOKEN_U32:
    case TOKEN_U64:
    case TOKEN_U128:
    case TOKEN_ISIZE:
    case TOKEN_USIZE:
    case TOKEN_F32:
    case TOKEN_F64:
    case TOKEN_CHAR:
    case TOKEN_STRING:
    case TOKEN_UNIT:
        return true;
    default:
        return false;
    }
}
