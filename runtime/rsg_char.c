#include "rsg_char.h"

bool rsg_char_is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool rsg_char_is_digit(char c) {
    return c >= '0' && c <= '9';
}

bool rsg_char_is_alphanumeric(char c) {
    return rsg_char_is_alpha(c) || rsg_char_is_digit(c);
}

bool rsg_char_is_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

bool rsg_char_is_upper(char c) {
    return c >= 'A' && c <= 'Z';
}

bool rsg_char_is_lower(char c) {
    return c >= 'a' && c <= 'z';
}

char rsg_char_to_upper(char c) {
    // NOLINTNEXTLINE(bugprone-narrowing-conversions)
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : (char)c;
}

char rsg_char_to_lower(char c) {
    // NOLINTNEXTLINE(bugprone-narrowing-conversions)
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
}

RsgStr rsg_char_to_str(char c) {
    return rsg_str_from_char(c);
}

int32_t rsg_char_to_i32(char c) {
    return (int32_t)(unsigned char)c;
}
