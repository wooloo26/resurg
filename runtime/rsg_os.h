#ifndef RSG_OS_H
#define RSG_OS_H

/**
 * @file rsg_os.h
 * @brief std/os module — OS interaction (env vars, args, exit).
 */

#include <stdbool.h>
#include <stdint.h>

#include "rsg_slice.h"
#include "rsg_str.h"

/** Get command-line arguments (excluding program name). */
RsgSlice rsg_os_args(void);
/** Get environment variable value. Returns empty string if not set. */
RsgStr rsg_os_env(RsgStr key);
/** Set environment variable. */
void rsg_os_set_env(RsgStr key, RsgStr value);
/** Exit the process with a status code. */
void rsg_os_exit(int32_t code);

/** Must be called from main() to capture argc/argv. */
void rsg_os_init(int argc, char **argv);

#endif // RSG_OS_H
