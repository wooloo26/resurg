#ifndef RSG_RUNTIME_H
#define RSG_RUNTIME_H

/**
 * @file rsg_runtime.h
 * @brief Resurg runtime library — umbrella header.
 *
 * Includes all runtime sub-modules.  Compiled programs include this
 * single header; each sub-module can also be included individually.
 */

#include "rsg_char.h"
#include "rsg_fmt.h"
#include "rsg_fn.h"
#include "rsg_fs.h"
#include "rsg_gc.h"
#include "rsg_io.h"
#include "rsg_math.h"
#include "rsg_os.h"
#include "rsg_panic.h"
#include "rsg_path.h"
#include "rsg_prim.h"
#include "rsg_rand.h"
#include "rsg_slice.h"
#include "rsg_sort.h"
#include "rsg_str.h"
#include "rsg_strmod.h"
#include "rsg_testing.h"
#include "rsg_time.h"

// Generated C code may use these directly (e.g. strlen in recover lowering).
#include <string.h>

#endif // RSG_RUNTIME_H
