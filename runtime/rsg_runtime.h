#ifndef RSG_RUNTIME_H
#define RSG_RUNTIME_H

/**
 * @file rsg_runtime.h
 * @brief Resurg runtime library — umbrella header.
 *
 * Includes all runtime sub-modules.  Compiled programs include this
 * single header; each sub-module can also be included individually.
 */

#include "rsg_fn.h"
#include "rsg_gc.h"
#include "rsg_io.h"
#include "rsg_panic.h"
#include "rsg_slice.h"
#include "rsg_str.h"

// Generated C code may use these directly (e.g. strlen in recover lowering).
#include <string.h>

#endif // RSG_RUNTIME_H
