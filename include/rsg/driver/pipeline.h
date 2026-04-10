#ifndef RSG_PIPELINE_H
#define RSG_PIPELINE_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @file pipeline.h
 * @brief Public pipeline facade — drives the full compilation pipeline.
 *
 * Hides all internal stages (lex, parse, resolve, check, mono, lower, codegen) behind
 * a single entry point so that callers depend only on this header.
 *
 * Individual stages can also be invoked independently through the
 * @c pipeline_lex, @c pipeline_parse, etc. functions.  This allows tools
 * (LSP, formatter, linter) to run only the stages they need and inspect
 * intermediate products.
 */
typedef struct Pipeline Pipeline;

/** Opaque handles for intermediate products exposed by stage functions. */
typedef struct Token Token;
typedef struct ASTNode ASTNode;
typedef struct HirNode HirNode;
typedef struct Sema Sema;
typedef struct CGenTarget CGenTarget;
typedef struct Arena Arena;
typedef struct DiagCtx DiagCtx;

/** Options controlling how the pipeline runs. */
typedef struct {
    const char *input_file;  // Mandatory .rsg src path.
    const char *output_file; // -o destination; NULL means stdout.
    const char *std_path;    // --std-path: explicit std library dir; NULL = auto-detect.
    const char *argv0;       // argv[0] for exe-relative std path discovery.
    bool dump_tokens;        // --dump-tokens: print token stream and exit.
    bool dump_ast;           // --dump-ast: pretty-print AST and exit.
    bool dump_hir;           // --dump-hir: pretty-print HIR and exit.
    bool no_prelude;         // --no-prelude: disable automatic prelude injection.
} PipelineOptions;

/** Create a pipeline inst. */
Pipeline *pipeline_create(void);
/** Destroy the pipeline and free all resrcs. */
void pipeline_destroy(Pipeline *pipeline);
/**
 * Run the full compilation pipeline: lex → parse → resolve → check → mono → lower → codegen.
 * Returns 0 on success, 1 on compilation errs.
 * Debug flags in @p options may short-circuit after an earlier stage.
 */
int pipeline_run(Pipeline *pipeline, const PipelineOptions *options);

// ── Composable stage API ───────────────────────────────────────────────

/**
 * @brief Access the pipeline's arena (valid during pipeline_run, or between
 *        pipeline_create and pipeline_destroy for manual stage invocation).
 */
Arena *pipeline_arena(Pipeline *pipeline);

/** Access the pipeline's HIR arena. */
Arena *pipeline_hir_arena(Pipeline *pipeline);

/** Access the pipeline's diagnostic context. */
DiagCtx *pipeline_diag_ctx(Pipeline *pipeline);

/**
 * Stage 1: Lex source text into a token stream.
 *
 * Returns a stretchy buf of tokens (use BUF_LEN for count).
 * The caller must BUF_FREE the result.  Returns NULL on lex errs.
 */
Token *pipeline_lex(Pipeline *pipeline, const char *src, const char *file);

/**
 * Stage 2: Parse a token stream into an AST.
 *
 * Returns the root NODE_FILE, or NULL on parse errs.
 * @p count is the number of tokens (BUF_LEN of the lex output).
 */
ASTNode *pipeline_parse(Pipeline *pipeline, Token *tokens, int32_t count, const char *file);

/**
 * Stage 3a: Resolve names and register declarations.
 *
 * Returns true on success.  Populates the Sema context with symbol tables.
 * Must be called before pipeline_check.
 */
bool pipeline_resolve(Pipeline *pipeline, Sema *sema, ASTNode *file);

/**
 * Stage 3b: Type-check the resolved AST.
 *
 * Returns true on success.  Sets type annotations on all AST nodes.
 * Must be called after pipeline_resolve.
 */
bool pipeline_check(Pipeline *pipeline, Sema *sema, ASTNode *file);

/**
 * Stage 4: Lower type-checked AST to HIR.
 *
 * Returns the root HIR_FILE node or NULL on failure.
 * Also runs HIR-to-HIR optimisation passes (const-fold, escape analysis).
 */
HirNode *pipeline_lower(Pipeline *pipeline, ASTNode *file);

/**
 * Stage 5: Emit code from HIR through the given backend target.
 *
 * @p target is a CGenTarget created via cgen_create() or a custom backend.
 */
void pipeline_emit(Pipeline *pipeline, CGenTarget *target, const HirNode *file);

#endif // RSG_PIPELINE_H
