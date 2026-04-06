#ifndef RG_CODEGEN_H
#define RG_CODEGEN_H

#include "types/tt.h"

/**
 * @file cgen.h
 * @brief Code generator - emits C17 source from a Typed Tree.
 *
 * Handles fn mangling, var shadowing, ternary optimisation,
 * and constant folding of integer exprs.
 */
typedef struct VarEntry VarEntry;
typedef struct CGen CGen;

/**
 * Create a code generator that writes to @p output.  Temporary strs are
 * allocated from @p arena.
 */
CGen *cgen_create(FILE *output, Arena *arena);
/** Destroy the generator and free its var-tracking buf. */
void cgen_destroy(CGen *cgen);
/**
 * Emit the full C translation unit for @p file (preamble, forward decls,
 * fn bodies, top-level stmts).
 */
void cgen_emit(CGen *cgen, const TTNode *file);

#endif // RG_CODEGEN_H
