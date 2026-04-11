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
void rsg_print_str(RsgStr src);
void rsg_print_i32(int32_t value);
void rsg_print_u32(uint32_t value);
void rsg_print_f64(double value);
void rsg_print_bool(bool value);
void rsg_print_char(char value);

/** Print to stdout with a trailing newline. */
void rsg_println_str(RsgStr src);
void rsg_println_i32(int32_t value);
void rsg_println_u32(uint32_t value);
void rsg_println_f64(double value);
void rsg_println_bool(bool value);
void rsg_println_char(char value);

// ── Extended I/O (std/io module) ──────────────────────────────────────

/** Read a line from stdin (without trailing newline). */
RsgStr rsg_io_read_line(void);
/** Print a string to stderr (no newline). */
void rsg_io_eprint(RsgStr msg);
/** Print a string to stderr with trailing newline. */
void rsg_io_eprintln(RsgStr msg);

#endif // RSG_IO_H
