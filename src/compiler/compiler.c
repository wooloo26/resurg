#include <sys/stat.h>

#ifndef S_ISREG
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif

#include "core/common.h"
#include "lexer/lexer.h"
#include "rsg/codegen.h"
#include "rsg/compiler.h"
#include "rsg/lowering.h"
#include "rsg/parser.h"
#include "rsg/sema.h"
#include "types/tt_passes.h"

/**
 * @file compiler.c
 * @brief Compiler facade — orchestrates all pipeline stages.
 */

/** Maximum source file size accepted by the compiler (64 MiB). */
#define MAX_SOURCE_SIZE (64L * 1024 * 1024)

struct Compiler {
    Arena *arena;
    Arena *tt_arena;
};

// ── File I/O ───────────────────────────────────────────────────────────

/**
 * Read the entire contents of @p path into a heap-allocated, NUL-terminated
 * buffer.  Fatally exits on I/O failure or if the file exceeds
 * MAX_SOURCE_SIZE.  The caller owns the returned memory.
 */
static char *read_source_file(const char *path) {
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
    if (size < 0 || size > MAX_SOURCE_SIZE) {
        fclose(file_handle);
        rsg_fatal("cannot read '%s' (size error or file too large)", path);
    }

    char *buffer = rsg_calloc((size_t)size + 1, 1);
    size_t bytes_read = fread(buffer, 1, (size_t)size, file_handle);
    fclose(file_handle);
    if (bytes_read != (size_t)size) {
        free(buffer);
        rsg_fatal("failed to read '%s' (expected %ld bytes, got %zu)", path, size, bytes_read);
    }
    return buffer;
}

// ── Pipeline stages ────────────────────────────────────────────────────

/** Run the lexer and optionally dump tokens.  Returns 0 on success, 1 on error. */
static int stage_lex(const CompilerOptions *options, const char *source, Arena *arena,
                     Token **out_tokens) {
    Lexer *lexer = lexer_create(source, options->input_file, arena);
    *out_tokens = lexer_scan_all(lexer);
    lexer_destroy(lexer);

    for (int32_t i = 0; i < BUFFER_LENGTH(*out_tokens); i++) {
        if ((*out_tokens)[i].kind == TOKEN_ERROR) {
            return 1;
        }
    }

    if (options->dump_tokens) {
        for (int32_t i = 0; i < BUFFER_LENGTH(*out_tokens); i++) {
            Token *token = &(*out_tokens)[i];
            fprintf(stderr, "%3d:%-3d  %-16s  '%.*s'\n", token->location.line,
                    token->location.column, token_kind_string(token->kind), token->length,
                    token->lexeme);
        }
        return -1; // sentinel: early exit requested
    }
    return 0;
}

/** Run the parser and optionally dump the AST.  Returns NULL on early exit. */
static ASTNode *stage_parse(const CompilerOptions *options, Token *tokens, int32_t count,
                            Arena *arena, int *out_status) {
    Parser *parser = parser_create(tokens, count, arena, options->input_file);
    ASTNode *file_node = parser_parse(parser);
    int32_t errors = parser_error_count(parser);
    parser_destroy(parser);

    if (options->dump_ast) {
        ast_dump(file_node, 0);
        return NULL;
    }
    if (errors > 0) {
        *out_status = 1;
        return NULL;
    }
    return file_node;
}

/** Run semantic analysis.  Returns true on success, false on errors. */
static bool stage_check(Arena *arena, ASTNode *file_node) {
    SemanticAnalyzer *analyzer = semantic_analyzer_create(arena);
    bool ok = semantic_analyzer_check(analyzer, file_node);
    semantic_analyzer_destroy(analyzer);
    return ok;
}

/** Lower the AST to typed tree, run TT passes, and optionally dump.  Returns NULL on early exit. */
static TtNode *stage_lower(const CompilerOptions *options, Arena *tt_arena, ASTNode *file_node,
                           Lowering **out_lowering) {
    *out_lowering = lowering_create(tt_arena);
    TtNode *tt_root = lowering_lower(*out_lowering, file_node);

    tt_pass_const_fold(tt_arena, tt_root);

    if (options->dump_tt) {
        tt_dump(tt_root, 0);
        return NULL;
    }
    return tt_root;
}

/** Run code generation, emitting C source to the output file. */
static void stage_emit(const CompilerOptions *options, Arena *arena, const TtNode *tt_root) {
    FILE *out = stdout;
    if (options->output_file != NULL) {
        out = fopen(options->output_file, "w");
        if (out == NULL) {
            rsg_fatal("cannot open output '%s'", options->output_file);
        }
    }
    CodeGenerator *code_generator = code_generator_create(out, arena);
    code_generator_emit(code_generator, tt_root);
    code_generator_destroy(code_generator);
    if (options->output_file != NULL) {
        fclose(out);
    }
}

// ── Public API ─────────────────────────────────────────────────────────

Compiler *compiler_create(void) {
    Compiler *compiler = rsg_malloc(sizeof(Compiler));
    compiler->arena = NULL;
    compiler->tt_arena = NULL;
    return compiler;
}

void compiler_destroy(Compiler *compiler) {
    free(compiler);
}

int compiler_run(Compiler *compiler, const CompilerOptions *options) {
    char *source = read_source_file(options->input_file);
    compiler->arena = arena_create();
    compiler->tt_arena = arena_create();
    Token *tokens = NULL; /* buf */
    TtNode *tt_root = NULL;
    Lowering *lowering = NULL;
    int status = 0;

    // Stage 1: Lexical analysis.
    status = stage_lex(options, source, compiler->arena, &tokens);
    if (status != 0) {
        status = (status == -1) ? 0 : status;
        goto cleanup;
    }

    // Stage 2: Parsing.
    ASTNode *file_node =
        stage_parse(options, tokens, BUFFER_LENGTH(tokens), compiler->arena, &status);
    if (file_node == NULL) {
        goto cleanup;
    }

    // Stage 3: Semantic analysis.
    if (!stage_check(compiler->arena, file_node)) {
        status = 1;
        goto cleanup;
    }

    // Stage 4: Lowering + TT passes.
    tt_root = stage_lower(options, compiler->tt_arena, file_node, &lowering);
    if (tt_root == NULL) {
        goto cleanup;
    }

    // Stage 5: Code generation.
    stage_emit(options, compiler->arena, tt_root);

cleanup:
    lowering_destroy(lowering);
    BUFFER_FREE(tokens);
    free(source);
    source = NULL;
    arena_destroy(compiler->tt_arena);
    arena_destroy(compiler->arena);
    compiler->arena = NULL;
    compiler->tt_arena = NULL;
    return status;
}
