#ifndef RSG_FS_H
#define RSG_FS_H

/**
 * @file rsg_fs.h
 * @brief std/fs module — file system operations.
 */

#include <stdbool.h>

#include "rsg_str.h"

/** Read an entire file to string. Returns empty string on error. */
RsgStr rsg_fs_read_file(RsgStr path);
/** Write a string to file (creates or truncates). Returns success. */
bool rsg_fs_write_file(RsgStr path, RsgStr content);
/** Append a string to file. Returns success. */
bool rsg_fs_append_file(RsgStr path, RsgStr content);
/** Check if a path exists. */
bool rsg_fs_exists(RsgStr path);
/** Remove a file. Returns success. */
bool rsg_fs_remove(RsgStr path);
/** Rename/move a file. Returns success. */
bool rsg_fs_rename(RsgStr old_path, RsgStr new_path);
/** Create a directory. Returns success. */
bool rsg_fs_mkdir(RsgStr path);
/** List entries in a directory. Returns a slice of str filenames. */
RsgSlice rsg_fs_read_dir(RsgStr path);

#endif // RSG_FS_H
