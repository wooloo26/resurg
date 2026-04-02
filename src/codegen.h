#ifndef RG_CODEGEN_H
#define RG_CODEGEN_H

#include "ast.h"
#include "types.h"

// ---------------------------------------------------------------------------
// Code generator — emits C17 source from a type-checked AST.
// ---------------------------------------------------------------------------
// Variable name mapping for shadowed variables
typedef struct VarEntry VarEntry;
typedef struct CodeGen CodeGen;

// Create a code generator writing to the given file.
CodeGen *codegen_create(FILE *out, Arena *arena);
// Destroy the code generator.
void codegen_destroy(CodeGen *cg);
// Emit C17 source for the type-checked AST.
void codegen_emit(CodeGen *cg, const ASTNode *file);

#endif // RG_CODEGEN_H
