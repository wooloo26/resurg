#include <sys/stat.h>

#ifndef S_ISREG
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif

#include "core/common.h"
#include "pass/lex/lex.h"
#include "pass/lower/hir_passes.h"
#include "rsg/driver/pipeline.h"
#include "rsg/pass/cgen/cgen.h"
#include "rsg/pass/check/check.h"
#include "rsg/pass/lower/lower.h"
#include "rsg/pass/mono/mono.h"
#include "rsg/pass/parse/parse.h"
#include "rsg/pass/resolve/resolve.h"

/**
 * @file pipeline.c
 * @brief Pipeline facade — orchestrates all pipeline stages.
 */

/** Maximum src file size accepted by the pipeline (64 MiB). */
#define MAX_SRC_SIZE (64L * 1024 * 1024)

#ifdef _WIN32
#define PATH_SEP '\\'
#else
#define PATH_SEP '/'
#endif

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

// ── Module loader callback ─────────────────────────────────────────────

/** Read a module file into a NUL-terminated heap buffer. Returns NULL on failure. */
static char *read_module_file(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return NULL;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    size_t size = (size_t)st.st_size;
    char *buf = rsg_calloc(size + 1, 1);
    size_t n = fread(buf, 1, size, f);
    fclose(f);
    if (n != size) {
        free(buf);
        return NULL;
    }
    return buf;
}

/**
 * Pipeline-provided module loader: lex and parse a module file.
 * @p ctx is unused (may be NULL).
 */
static ASTNode **pipeline_load_module(void *ctx, Arena *arena, const char *mod_path) {
    (void)ctx;
    char *src = read_module_file(mod_path);
    if (src == NULL) {
        return NULL;
    }
    Lex *lex = lex_create(src, mod_path, arena);
    Token *tokens = lex_scan_all(lex);
    lex_destroy(lex);

    int32_t count = BUF_LEN(tokens);
    Parser *parser = parser_create(tokens, count, arena, mod_path);
    ASTNode *file_node = parser_parse(parser);
    int32_t errs = parser_err_count(parser);
    parser_destroy(parser);
    free(src);

    if (errs > 0 || file_node == NULL) {
        return NULL;
    }
    return file_node->file.decls;
}

// ── Pipeline stages ────────────────────────────────────────────────────

/** Extract the directory portion of @p file_path into @p arena. */
static const char *pipeline_dir_of(Arena *arena, const char *file_path) {
    const char *last_sep = strrchr(file_path, '/');
#ifdef _WIN32
    const char *last_bsep = strrchr(file_path, '\\');
    if (last_bsep != NULL && (last_sep == NULL || last_bsep > last_sep)) {
        last_sep = last_bsep;
    }
#endif
    if (last_sep == NULL) {
        return arena_strdup(arena, ".");
    }
    size_t len = (size_t)(last_sep - file_path);
    char *dir = arena_alloc(arena, len + 1);
    memcpy(dir, file_path, len);
    dir[len] = '\0';
    return dir;
}

/** Return true if @p path names an existing regular file. */
static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/**
 * Discover the std library directory.
 *
 * Search order (first hit wins):
 *   1. Explicit --std-path=<dir>
 *   2. RSG_STD_PATH environment variable
 *   3. <exe_dir>/../std/  (development layout)
 *   4. <exe_dir>/../lib/rsg/std/  (installed layout)
 *
 * Returns NULL when no std directory is found.
 */
static const char *find_std_path(Arena *arena, const PipelineOptions *options) {
    // 1. Explicit --std-path=<dir>
    if (options->std_path != NULL) {
        return options->std_path;
    }

    // 2. RSG_STD_PATH environment variable
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const char *env = getenv("RSG_STD_PATH");
    if (env != NULL && env[0] != '\0') {
        return env;
    }

    // 3 & 4. Relative to the compiler executable
    if (options->argv0 != NULL) {
        const char *exe_dir = pipeline_dir_of(arena, options->argv0);

        // Development layout: <exe_dir>/../std/
        const char *dev = arena_sprintf(arena, "%s%c..%cstd", exe_dir, PATH_SEP, PATH_SEP);
        const char *probe = arena_sprintf(arena, "%s%cprelude.rsg", dev, PATH_SEP);
        if (file_exists(probe)) {
            return dev;
        }

        // Installed layout: <exe_dir>/../lib/rsg/std/
        const char *inst = arena_sprintf(arena, "%s%c..%clib%crsg%cstd", exe_dir, PATH_SEP,
                                         PATH_SEP, PATH_SEP, PATH_SEP);
        probe = arena_sprintf(arena, "%s%cprelude.rsg", inst, PATH_SEP);
        if (file_exists(probe)) {
            return inst;
        }
    }

    return NULL;
}

/**
 * Parse an .rsg file from @p path and prepend its declarations to @p file_node.
 *
 * Returns true when at least one declaration was prepended.
 */
static bool prepend_rsg_file(Arena *arena, const char *path, ASTNode *file_node) {
    char *src = read_module_file(path);
    if (src == NULL) {
        return false;
    }

    Lex *lex = lex_create(src, path, arena);
    Token *tokens = lex_scan_all(lex);
    lex_destroy(lex);

    int32_t count = BUF_LEN(tokens);
    Parser *parser = parser_create(tokens, count, arena, path);
    ASTNode *parsed = parser_parse(parser);
    int32_t errs = parser_err_count(parser);
    parser_destroy(parser);
    free(src);

    if (errs > 0 || parsed == NULL) {
        return false;
    }

    int32_t parsed_count = BUF_LEN(parsed->file.decls);
    if (parsed_count == 0) {
        return false;
    }

    int32_t user_count = BUF_LEN(file_node->file.decls);
    ASTNode **merged = NULL;
    for (int32_t i = 0; i < parsed_count; i++) {
        BUF_PUSH(merged, parsed->file.decls[i]);
    }
    for (int32_t i = 0; i < user_count; i++) {
        BUF_PUSH(merged, file_node->file.decls[i]);
    }

    BUF_FREE(file_node->file.decls);
    file_node->file.decls = merged;
    return true;
}

/**
 * Always-inject std/builtin.rsg (core types and intrinsic declarations).
 *
 * Returns the discovered std directory path (for module resolution fallback),
 * or NULL when the std directory is unavailable.
 */
static const char *inject_builtin(Arena *arena, const PipelineOptions *options,
                                  ASTNode *file_node) {
    const char *std_dir = find_std_path(arena, options);
    if (std_dir == NULL) {
        return NULL;
    }

    const char *builtin_path = arena_sprintf(arena, "%s%cbuiltin.rsg", std_dir, PATH_SEP);
    prepend_rsg_file(arena, builtin_path, file_node);
    return std_dir;
}

/**
 * Optionally inject std/prelude.rsg (convenience re-exports).
 *
 * Skipped when --no-prelude is set.
 */
static void inject_prelude(Arena *arena, const PipelineOptions *options, const char *std_dir,
                           ASTNode *file_node) {
    if (options->no_prelude || std_dir == NULL) {
        return;
    }

    const char *prelude_path = arena_sprintf(arena, "%s%cprelude.rsg", std_dir, PATH_SEP);
    prepend_rsg_file(arena, prelude_path, file_node);
}

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
static bool stage_sema(Arena *arena, ASTNode *file_node, const char *std_search_dir) {
    Sema *sema = sema_create(arena);
    sema_set_module_loader(sema, pipeline_load_module, NULL);
    if (std_search_dir != NULL) {
        sema_set_std_search_dir(sema, std_search_dir);
    }
    bool ok = sema_resolve(sema, file_node) && sema_check(sema, file_node) &&
              sema_mono(sema, file_node, sema_check_fn_body);
    sema_destroy(sema);
    return ok;
}

/** Lower the AST to HIR and optionally dump.  Returns NULL on early exit. */
static HirNode *stage_lower(const PipelineOptions *options, Arena *hir_arena, ASTNode *file_node,
                            Lower **out_lower) {
    *out_lower = lower_create(hir_arena);
    HirNode *hir_root = lower_lower(*out_lower, file_node);

    // Run HIR-to-HIR optimization passes.
    hir_pass_const_fold(hir_arena, hir_root);
    hir_pass_escape_analysis(hir_arena, hir_root);

    if (options->dump_hir) {
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

    // Stage 2.5a: Always-inject std/builtin.rsg (core types and intrinsics).
    const char *std_dir = inject_builtin(pipeline->arena, options, file_node);

    // Stage 2.5b: Optionally inject std/prelude.rsg (convenience re-exports).
    inject_prelude(pipeline->arena, options, std_dir, file_node);

    // Stage 3: Semantic analysis (resolve → check → mono).
    if (!stage_sema(pipeline->arena, file_node, std_dir)) {
        status = 1;
        goto cleanup;
    }

    // Stage 4: Lower + HIR passes.
    hir_root = stage_lower(options, pipeline->hir_arena, file_node, &lower);
    if (hir_root == NULL) {
        goto cleanup;
    }

    // Stage 5: Code generation.
    stage_emit(options, pipeline->arena, hir_root);

cleanup:
    lower_destroy(lower);
    BUF_FREE(tokens);
    free(src);
    src = NULL;
    arena_destroy(pipeline->hir_arena);
    arena_destroy(pipeline->arena);
    pipeline->arena = NULL;
    pipeline->hir_arena = NULL;
    return status;
}
