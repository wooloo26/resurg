#ifndef RG_CODEGEN_H
#define RG_CODEGEN_H

#include "types/type_tree.h"

/**
 * @file codegen.h
 * @brief Code generator - emits C17 source from a Typed Tree.
 *
 * Handles function mangling, variable shadowing, ternary optimisation,
 * and constant folding of integer expressions.
 */
typedef struct VariableEntry VariableEntry;
typedef struct CodeGenerator CodeGenerator;

/**
 * Create a code generator that writes to @p output.  Temporary strings are
 * allocated from @p arena.
 */
CodeGenerator *code_generator_create(FILE *output, Arena *arena);
/** Destroy the generator and free its variable-tracking buffer. */
void code_generator_destroy(CodeGenerator *generator);
/**
 * Emit the full C translation unit for @p file (preamble, forward decls,
 * function bodies, top-level statements).
 */
void code_generator_emit(CodeGenerator *generator, const TtNode *file);

#endif // RG_CODEGEN_H
