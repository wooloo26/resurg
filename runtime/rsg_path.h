#ifndef RSG_PATH_H
#define RSG_PATH_H

/**
 * @file rsg_path.h
 * @brief std/path module — pure string-based path manipulation.
 */

#include <stdbool.h>

#include "rsg_str.h"

/** Join two path segments with the platform separator. */
RsgStr rsg_path_join(RsgStr a, RsgStr b);
/** Extract the file name from a path. */
RsgStr rsg_path_file_name(RsgStr path);
/** Extract the directory portion. */
RsgStr rsg_path_dir(RsgStr path);
/** Extract the file extension (without leading dot), or empty. */
RsgStr rsg_path_extension(RsgStr path);
/** Check if a path is absolute. */
bool rsg_path_is_absolute(RsgStr path);

#endif // RSG_PATH_H
