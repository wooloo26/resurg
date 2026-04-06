#ifndef RG_COMPILER_H
#define RG_COMPILER_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @file compiler.h
 * @brief Public compiler facade — drives the full compilation pipeline.
 *
 * Hides all internal stages (lexer, parser, sema, lowering, codegen) behind
 * a single entry point so that callers depend only on this header.
 */
typedef struct Compiler Compiler;

/** Options controlling how the compiler runs. */
typedef struct {
    const char *input_file;  // Mandatory .rsg source path.
    const char *output_file; // -o destination; NULL means stdout.
    bool dump_tokens;        // --dump-tokens: print token stream and exit.
    bool dump_ast;           // --dump-ast: pretty-print AST and exit.
    bool dump_tt;            // --dump-tt: pretty-print Typed Tree and exit.
} CompilerOptions;

/** Create a compiler inst. */
Compiler *compiler_create(void);
/** Destroy the compiler and free all resources. */
void compiler_destroy(Compiler *compiler);
/**
 * Run the full compilation pipeline: lex → parse → sema → lower → codegen.
 * Returns 0 on success, 1 on compilation errs.
 * Debug flags in @p options may short-circuit after an earlier stage.
 */
int compiler_run(Compiler *compiler, const CompilerOptions *options);

#endif // RG_COMPILER_H
