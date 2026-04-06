#ifndef RSG_CGEN_H
#define RSG_CGEN_H

#include "repr/hir.h"

/**
 * @file cgen.h
 * @brief Code generator — public API.
 *
 * Creates a backend target and emits C17 src from a HIR tree.
 * The default backend targets C; additional backends (C++, …) may be
 * added by implementing the CGenTarget contract in target.h.
 */
typedef struct CGenTarget CGenTarget;

/**
 * Create a C-backend code generator that writes to @p output.
 * Temporary strs are allocated from @p arena.
 */
CGenTarget *cgen_create(FILE *output, Arena *arena);
/** Destroy the target and free its owned resrcs. */
void cgen_destroy(CGenTarget *target);
/**
 * Emit the full C translation unit for @p file (preamble, forward decls,
 * fn bodies, top-level stmts).
 */
void cgen_emit(CGenTarget *target, const HirNode *file);

#endif // RSG_CGEN_H
