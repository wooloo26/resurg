#include "token.h"

// Token classification flags.
enum {
    TKF_TYPE_KEYWORD = 1 << 0,
    TKF_YIELDS_BOOL = 1 << 1,
    TKF_COMPOUND_ASSIGN = 1 << 2,
};

/**
 * Unified descriptor table for all token kinds.  Adding a new token
 * requires a single entry here — no parallel switch or array to update.
 */
typedef struct {
    const char *name;
    uint8_t flags;
    uint8_t precedence; // Precedence enum value
    TokenKind base_op;  // For compound assigns: TOKEN_PLUS_EQUAL → TOKEN_PLUS
} TokenDescriptor;

static const TokenDescriptor TOKEN_TABLE[] = {
    [TOKEN_INTEGER_LIT] = {"INTEGER_LIT", 0, 0, 0},
    [TOKEN_FLOAT_LIT] = {"FLOAT_LIT", 0, 0, 0},
    [TOKEN_STR_LIT] = {"STR_LIT", 0, 0, 0},
    [TOKEN_CHAR_LIT] = {"CHAR_LIT", 0, 0, 0},
    [TOKEN_TRUE] = {"true", 0, 0, 0},
    [TOKEN_FALSE] = {"false", 0, 0, 0},
    [TOKEN_ID] = {"ID", 0, 0, 0},
    [TOKEN_MODULE] = {"mod", 0, 0, 0},
    [TOKEN_PUB] = {"pub", 0, 0, 0},
    [TOKEN_FN] = {"fn", 0, 0, 0},
    [TOKEN_VAR] = {"var", 0, 0, 0},
    [TOKEN_IF] = {"if", 0, 0, 0},
    [TOKEN_ELSE] = {"else", 0, 0, 0},
    [TOKEN_LOOP] = {"loop", 0, 0, 0},
    [TOKEN_FOR] = {"for", 0, 0, 0},
    [TOKEN_BREAK] = {"break", 0, 0, 0},
    [TOKEN_CONTINUE] = {"continue", 0, 0, 0},
    [TOKEN_TYPE] = {"type", 0, 0, 0},
    [TOKEN_STRUCT] = {"struct", 0, 0, 0},
    [TOKEN_MUT] = {"mut", 0, 0, 0},
    [TOKEN_IMMUT] = {"immut", 0, 0, 0},
    [TOKEN_ENUM] = {"enum", 0, 0, 0},
    [TOKEN_PACT] = {"pact", 0, 0, 0},
    [TOKEN_MATCH] = {"match", 0, 0, 0},
    [TOKEN_RETURN] = {"return", 0, 0, 0},
    [TOKEN_WHILE] = {"while", 0, 0, 0},
    [TOKEN_DEFER] = {"defer", 0, 0, 0},
    [TOKEN_EXT] = {"ext", 0, 0, 0},
    [TOKEN_USE] = {"use", 0, 0, 0},
    [TOKEN_IMPL] = {"impl", 0, 0, 0},
    [TOKEN_WHERE] = {"where", 0, 0, 0},
    [TOKEN_COMPTIME] = {"comptime", 0, 0, 0},
    [TOKEN_SELF] = {"Self", 0, 0, 0},
    [TOKEN_BOOL] = {"bool", TKF_TYPE_KEYWORD, 0, 0},
    [TOKEN_I8] = {"i8", TKF_TYPE_KEYWORD, 0, 0},
    [TOKEN_I16] = {"i16", TKF_TYPE_KEYWORD, 0, 0},
    [TOKEN_I32] = {"i32", TKF_TYPE_KEYWORD, 0, 0},
    [TOKEN_I64] = {"i64", TKF_TYPE_KEYWORD, 0, 0},
    [TOKEN_I128] = {"i128", TKF_TYPE_KEYWORD, 0, 0},
    [TOKEN_U8] = {"u8", TKF_TYPE_KEYWORD, 0, 0},
    [TOKEN_U16] = {"u16", TKF_TYPE_KEYWORD, 0, 0},
    [TOKEN_U32] = {"u32", TKF_TYPE_KEYWORD, 0, 0},
    [TOKEN_U64] = {"u64", TKF_TYPE_KEYWORD, 0, 0},
    [TOKEN_U128] = {"u128", TKF_TYPE_KEYWORD, 0, 0},
    [TOKEN_ISIZE] = {"isize", TKF_TYPE_KEYWORD, 0, 0},
    [TOKEN_USIZE] = {"usize", TKF_TYPE_KEYWORD, 0, 0},
    [TOKEN_F32] = {"f32", TKF_TYPE_KEYWORD, 0, 0},
    [TOKEN_F64] = {"f64", TKF_TYPE_KEYWORD, 0, 0},
    [TOKEN_CHAR] = {"char", TKF_TYPE_KEYWORD, 0, 0},
    [TOKEN_STR] = {"str", TKF_TYPE_KEYWORD, 0, 0},
    [TOKEN_UNIT] = {"unit", TKF_TYPE_KEYWORD, 0, 0},
    [TOKEN_NEVER] = {"never", TKF_TYPE_KEYWORD, 0, 0},
    [TOKEN_PLUS] = {"+", 0, PRECEDENCE_TERM, 0},
    [TOKEN_MINUS] = {"-", 0, PRECEDENCE_TERM, 0},
    [TOKEN_STAR] = {"*", 0, PRECEDENCE_FACTOR, 0},
    [TOKEN_SLASH] = {"/", 0, PRECEDENCE_FACTOR, 0},
    [TOKEN_PERCENT] = {"%", 0, PRECEDENCE_FACTOR, 0},
    [TOKEN_EQUAL_EQUAL] = {"==", TKF_YIELDS_BOOL, PRECEDENCE_EQUALITY, 0},
    [TOKEN_BANG_EQUAL] = {"!=", TKF_YIELDS_BOOL, PRECEDENCE_EQUALITY, 0},
    [TOKEN_LESS] = {"<", TKF_YIELDS_BOOL, PRECEDENCE_COMPARISON, 0},
    [TOKEN_LESS_EQUAL] = {"<=", TKF_YIELDS_BOOL, PRECEDENCE_COMPARISON, 0},
    [TOKEN_GREATER] = {">", TKF_YIELDS_BOOL, PRECEDENCE_COMPARISON, 0},
    [TOKEN_GREATER_EQUAL] = {">=", TKF_YIELDS_BOOL, PRECEDENCE_COMPARISON, 0},
    [TOKEN_AMPERSAND] = {"&", 0, 0, 0},
    [TOKEN_AMPERSAND_AMPERSAND] = {"&&", TKF_YIELDS_BOOL, PRECEDENCE_AND, 0},
    [TOKEN_PIPE_PIPE] = {"||", TKF_YIELDS_BOOL, PRECEDENCE_OR, 0},
    [TOKEN_BANG] = {"!", 0, 0, 0},
    [TOKEN_PIPE] = {"|", 0, 0, 0},
    [TOKEN_PIPE_GREATER] = {"|>", 0, PRECEDENCE_PIPE, 0},
    [TOKEN_COLON_EQUAL] = {":=", 0, 0, 0},
    [TOKEN_EQUAL] = {"=", 0, PRECEDENCE_ASSIGN, 0},
    [TOKEN_PLUS_EQUAL] = {"+=", TKF_COMPOUND_ASSIGN, PRECEDENCE_ASSIGN, TOKEN_PLUS},
    [TOKEN_MINUS_EQUAL] = {"-=", TKF_COMPOUND_ASSIGN, PRECEDENCE_ASSIGN, TOKEN_MINUS},
    [TOKEN_STAR_EQUAL] = {"*=", TKF_COMPOUND_ASSIGN, PRECEDENCE_ASSIGN, TOKEN_STAR},
    [TOKEN_SLASH_EQUAL] = {"/=", TKF_COMPOUND_ASSIGN, PRECEDENCE_ASSIGN, TOKEN_SLASH},
    [TOKEN_LEFT_PAREN] = {"(", 0, 0, 0},
    [TOKEN_RIGHT_PAREN] = {")", 0, 0, 0},
    [TOKEN_LEFT_BRACE] = {"{", 0, 0, 0},
    [TOKEN_RIGHT_BRACE] = {"}", 0, 0, 0},
    [TOKEN_LEFT_BRACKET] = {"[", 0, 0, 0},
    [TOKEN_RIGHT_BRACKET] = {"]", 0, 0, 0},
    [TOKEN_COLON_COLON] = {"::", 0, 0, 0},
    [TOKEN_COLON] = {":", 0, 0, 0},
    [TOKEN_COMMA] = {",", 0, 0, 0},
    [TOKEN_DOT_DOT_EQUAL] = {"..=", 0, 0, 0},
    [TOKEN_DOT_DOT] = {"..", 0, 0, 0},
    [TOKEN_DOT] = {".", 0, 0, 0},
    [TOKEN_FAT_ARROW] = {"=>", 0, 0, 0},
    [TOKEN_ARROW] = {"->", 0, 0, 0},
    [TOKEN_SEMICOLON] = {";", 0, 0, 0},
    [TOKEN_QUESTION] = {"?", 0, 0, 0},
    [TOKEN_QUESTION_DOT] = {"?.", 0, 0, 0},
    [TOKEN_INTERPOLATION_START] = {"INTERPOLATION_START", 0, 0, 0},
    [TOKEN_INTERPOLATION_END] = {"INTERPOLATION_END", 0, 0, 0},
    [TOKEN_NEWLINE] = {"NEWLINE", 0, 0, 0},
    [TOKEN_EOF] = {"EOF", 0, 0, 0},
    [TOKEN_ERR] = {"ERR", 0, 0, 0},
};

static const int32_t TOKEN_TABLE_COUNT = (int32_t)(sizeof(TOKEN_TABLE) / sizeof(TOKEN_TABLE[0]));

const char *token_kind_str(TokenKind kind) {
    if (kind >= 0 && kind < TOKEN_TABLE_COUNT && TOKEN_TABLE[kind].name != NULL) {
        return TOKEN_TABLE[kind].name;
    }
    return "?";
}

bool token_is_type_keyword(TokenKind kind) {
    if (kind >= 0 && kind < TOKEN_TABLE_COUNT) {
        return (TOKEN_TABLE[kind].flags & TKF_TYPE_KEYWORD) != 0;
    }
    return false;
}

Precedence token_precedence(TokenKind kind) {
    if (kind >= 0 && kind < TOKEN_TABLE_COUNT) {
        return (Precedence)TOKEN_TABLE[kind].precedence;
    }
    return PRECEDENCE_NONE;
}

bool token_op_yields_bool(TokenKind op) {
    if (op >= 0 && op < TOKEN_TABLE_COUNT) {
        return (TOKEN_TABLE[op].flags & TKF_YIELDS_BOOL) != 0;
    }
    return false;
}

bool token_is_compound_assign(TokenKind op) {
    if (op >= 0 && op < TOKEN_TABLE_COUNT) {
        return (TOKEN_TABLE[op].flags & TKF_COMPOUND_ASSIGN) != 0;
    }
    return false;
}

TokenKind token_compound_base_op(TokenKind op) {
    if (op >= 0 && op < TOKEN_TABLE_COUNT && (TOKEN_TABLE[op].flags & TKF_COMPOUND_ASSIGN) != 0) {
        return TOKEN_TABLE[op].base_op;
    }
    return op;
}
