#ifndef RSG_CGEN_TARSGET_H
#define RSG_CGEN_TARSGET_H

#include "repr/hir.h"

/**
 * @file target.h
 * @brief Unified backend interface for code generation.
 *
 * Each backend (C, C++, …) embeds a CGenTarget as its first member
 * and populates the vtable.  The driver calls through the vtable
 * without knowing the concrete backend type.
 */
typedef struct CGenTarget CGenTarget;

struct CGenTarget {
    /** Emit a full translation unit for @p file. */
    void (*emit)(CGenTarget *self, const HirNode *file);
    /** Destroy the target and free all owned resrcs. */
    void (*destroy)(CGenTarget *self);
};

#endif // RSG_CGEN_TARSGET_H
