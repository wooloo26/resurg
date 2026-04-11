#ifndef RSG_CHAR_H
#define RSG_CHAR_H

/**
 * @file rsg_char.h
 * @brief Extension methods for the char primitive type.
 */

#include <stdbool.h>
#include <stdint.h>

#include "rsg_str.h"

bool rsg_char_is_alpha(char c);
bool rsg_char_is_digit(char c);
bool rsg_char_is_alphanumeric(char c);
bool rsg_char_is_whitespace(char c);
bool rsg_char_is_upper(char c);
bool rsg_char_is_lower(char c);
char rsg_char_to_upper(char c);
char rsg_char_to_lower(char c);
RsgStr rsg_char_to_str(char c);
int32_t rsg_char_to_i32(char c);

#endif // RSG_CHAR_H
