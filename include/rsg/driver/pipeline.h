#ifndef RSG_PIPELINE_H
#define RSG_PIPELINE_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @file pipeline.h
 * @brief Public pipeline facade — drives the full compilation pipeline.
 *
 * Hides all internal stages (lex, parser, sema, lower, codegen) behind
 * a single entry point so that callers depend only on this header.
 */
typedef struct Pipeline Pipeline;

/** Options controlling how the pipeline runs. */
typedef struct {
    const char *input_file;  // Mandatory .rsg src path.
    const char *output_file; // -o destination; NULL means stdout.
    bool dump_tokens;        // --dump-tokens: print token stream and exit.
    bool dump_ast;           // --dump-ast: pretty-print AST and exit.
    bool dump_hir;           // --dump-hir: pretty-print HIR and exit.
} PipelineOptions;

/** Create a pipeline inst. */
Pipeline *pipeline_create(void);
/** Destroy the pipeline and free all resrcs. */
void pipeline_destroy(Pipeline *pipeline);
/**
 * Run the full compilation pipeline: lex → parse → sema → lower → codegen.
 * Returns 0 on success, 1 on compilation errs.
 * Debug flags in @p options may short-circuit after an earlier stage.
 */
int pipeline_run(Pipeline *pipeline, const PipelineOptions *options);

#endif // RSG_PIPELINE_H
