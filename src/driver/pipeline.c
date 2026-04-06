#include <sys/stat.h>

#ifndef S_ISREG
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif

#include "core/common.h"
#include "pass/lex/lex.h"
#include "rsg/driver/pipeline.h"
#include "rsg/pass/cgen/cgen.h"
#include "rsg/pass/check/check.h"
#include "rsg/pass/lower/lower.h"
#include "rsg/pass/parse/parse.h"

/**
 * @file pipeline.c
 * @brief Pipeline facade — orchestrates all pipeline stages.
 */

/** Maximum src file size accepted by the pipeline (64 MiB). */
#define MAX_SRC_SIZE (64L * 1024 * 1024)

struct Pipeline {
    Arena *arena;
    Arena *hir_arena;
};

// ── File I/O ───────────────────────────────────────────────────────────

/**
 * Read the entire contents of @p path into a heap-allocated, NUL-terminated
 * buf.  Fatally exits on I/O failure or if the file exceeds
 * MAX_SRC_SIZE.  The caller owns the returned memory.
 */
static char *read_src_file(const char *path) {
    struct stat file_stat;
    if (stat(path, &file_stat) != 0) {
        rsg_fatal("cannot stat '%s'", path);
    }
    if (!S_ISREG(file_stat.st_mode)) {
        rsg_fatal("'%s' is not a regular file", path);
    }

    FILE *file_handle = fopen(path, "rb");
    if (file_handle == NULL) {
        rsg_fatal("cannot open '%s'", path);
    }

    long size = (long)file_stat.st_size;
    if (size < 0 || size > MAX_SRC_SIZE) {
        fclose(file_handle);
        rsg_fatal("cannot read '%s' (size err or file too large)", path);
    }

    char *buf = rsg_calloc((size_t)size + 1, 1);
    size_t bytes_read = fread(buf, 1, (size_t)size, file_handle);
    fclose(file_handle);
    if (bytes_read != (size_t)size) {
        free(buf);
        rsg_fatal("failed to read '%s' (expected %ld bytes, got %zu)", path, size, bytes_read);
    }
    return buf;
}

// ── Pipeline stages ────────────────────────────────────────────────────

/** Run the lex and optionally dump tokens.  Returns 0 on success, 1 on err. */
static int stage_lex(const PipelineOptions *options, const char *src, Arena *arena,
                     Token **out_tokens) {
    Lex *lex = lex_create(src, options->input_file, arena);
    *out_tokens = lex_scan_all(lex);
    lex_destroy(lex);

    for (int32_t i = 0; i < BUF_LEN(*out_tokens); i++) {
        if ((*out_tokens)[i].kind == TOKEN_ERR) {
            return 1;
        }
    }

    if (options->dump_tokens) {
        for (int32_t i = 0; i < BUF_LEN(*out_tokens); i++) {
            Token *token = &(*out_tokens)[i];
            fprintf(stderr, "%3d:%-3d  %-16s  '%.*s'\n", token->loc.line, token->loc.column,
                    token_kind_str(token->kind), token->len, token->lexeme);
        }
        return -1; // sentinel: early exit requested
    }
    return 0;
}

/** Run the parser and optionally dump the AST.  Returns NULL on early exit. */
static ASTNode *stage_parse(const PipelineOptions *options, Token *tokens, int32_t count,
                            Arena *arena, int *out_status) {
    Parser *parser = parser_create(tokens, count, arena, options->input_file);
    ASTNode *file_node = parser_parse(parser);
    int32_t errs = parser_err_count(parser);
    parser_destroy(parser);

    if (options->dump_ast) {
        ast_dump(file_node, 0);
        return NULL;
    }
    if (errs > 0) {
        *out_status = 1;
        return NULL;
    }
    return file_node;
}

/** Run semantic analysis.  Returns true on success, false on errs. */
static bool stage_check(Arena *arena, ASTNode *file_node) {
    Sema *sema = sema_create(arena);
    bool ok = sema_check(sema, file_node);
    sema_destroy(sema);
    return ok;
}

/** Lower the AST to typed tree, run TT passes, and optionally dump.  Returns NULL on early exit. */
static HirNode *stage_lower(const PipelineOptions *options, Arena *hir_arena, ASTNode *file_node,
                            Lower **out_lowering) {
    *out_lowering = lowering_create(hir_arena);
    HirNode *hir_root = lowering_lower(*out_lowering, file_node);

    if (options->dump_tt) {
        hir_dump(hir_root, 0);
        return NULL;
    }
    return hir_root;
}

/** Run code generation, emitting C src to the output file. */
static void stage_emit(const PipelineOptions *options, Arena *arena, const HirNode *hir_root) {
    FILE *out = stdout;
    if (options->output_file != NULL) {
        out = fopen(options->output_file, "w");
        if (out == NULL) {
            rsg_fatal("cannot open output '%s'", options->output_file);
        }
    }
    CGenTarget *target = cgen_create(out, arena);
    cgen_emit(target, hir_root);
    cgen_destroy(target);
    if (options->output_file != NULL) {
        fclose(out);
    }
}

// ── Public API ─────────────────────────────────────────────────────────

Pipeline *pipeline_create(void) {
    Pipeline *pipeline = rsg_malloc(sizeof(Pipeline));
    pipeline->arena = NULL;
    pipeline->hir_arena = NULL;
    return pipeline;
}

void pipeline_destroy(Pipeline *pipeline) {
    free(pipeline);
}

int pipeline_run(Pipeline *pipeline, const PipelineOptions *options) {
    char *src = read_src_file(options->input_file);
    pipeline->arena = arena_create();
    pipeline->hir_arena = arena_create();
    Token *tokens = NULL; /* buf */
    HirNode *hir_root = NULL;
    Lower *lower = NULL;
    int status = 0;

    // Stage 1: Lexical analysis.
    status = stage_lex(options, src, pipeline->arena, &tokens);
    if (status != 0) {
        status = (status == -1) ? 0 : status;
        goto cleanup;
    }

    // Stage 2: Parsing.
    ASTNode *file_node = stage_parse(options, tokens, BUF_LEN(tokens), pipeline->arena, &status);
    if (file_node == NULL) {
        goto cleanup;
    }

    // Stage 3: Semantic analysis.
    if (!stage_check(pipeline->arena, file_node)) {
        status = 1;
        goto cleanup;
    }

    // Stage 4: Lower + TT passes.
    hir_root = stage_lower(options, pipeline->hir_arena, file_node, &lower);
    if (hir_root == NULL) {
        goto cleanup;
    }

    // Stage 5: Code generation.
    stage_emit(options, pipeline->arena, hir_root);

cleanup:
    lowering_destroy(lower);
    BUF_FREE(tokens);
    free(src);
    src = NULL;
    arena_destroy(pipeline->hir_arena);
    arena_destroy(pipeline->arena);
    pipeline->arena = NULL;
    pipeline->hir_arena = NULL;
    return status;
}
