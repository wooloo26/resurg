#ifndef RSG_IO_H
#define RSG_IO_H

/**
 * @file rsg_io.h
 * @brief Typed I/O — print values to stdout.
 */

#include <stdbool.h>
#include <stdint.h>

#include "rsg_str.h"

/** Print an RsgStr to stdout (no trailing newline). */
void rsgu_print_str(RsgStr src);
void rsgu_print_i32(int32_t value);
void rsgu_print_u32(uint32_t value);
void rsgu_print_f64(double value);
void rsgu_print_bool(bool value);
void rsgu_print_char(char value);

/** Print to stdout with a trailing newline. */
void rsgu_println_str(RsgStr src);
void rsgu_println_i32(int32_t value);
void rsgu_println_u32(uint32_t value);
void rsgu_println_f64(double value);
void rsgu_println_bool(bool value);
void rsgu_println_char(char value);

#endif // RSG_IO_H
