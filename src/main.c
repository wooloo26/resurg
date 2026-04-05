#include <sys/stat.h>

#ifndef S_ISREG
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif

#include "core/common.h"
#include "lexer/lexer.h"
#include "rsg/codegen.h"
#include "rsg/lowering.h"
#include "rsg/parser.h"
#include "rsg/sema.h"
#include "types/tt_passes.h"

/** Maximum source file size accepted by the compiler (64 MiB). */
#define MAX_SOURCE_SIZE (64L * 1024 * 1024)

/**
 * Read the entire contents of @p path into a heap-allocated, NUL-terminated
 * buffer.  Fatally exits on I/O failure or if the file exceeds
 * MAX_SOURCE_SIZE.  The caller owns the returned memory.
 */
static char *read_file(const char *path) {
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

/** Print a usage synopsis to stderr and terminate with exit code 1. */
static noreturn void usage(void) {
    fprintf(stderr, "Usage: resurg <input.rsg> [options]\n"
                    "\n"
                    "Options:\n"
                    "  -o <file>     Output C file (default: stdout)\n"
                    "  --dump-tokens Print token stream and exit\n"
                    "  --dump-ast    Print AST and exit\n"
                    "  --dump-tt     Print Typed Tree and exit\n"
                    "  --help        Show this message\n");
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    exit(1);
}

/** Parsed command-line options forwarded to the compilation pipeline. */
typedef struct {
    const char *input_file;  // Mandatory .rsg source path.
    const char *output_file; // -o destination; NULL means stdout.
    bool dump_tokens;        // --dump-tokens: print token stream and exit.
    bool dump_ast;           // --dump-ast: pretty-print AST and exit.
    bool dump_tt;            // --dump-tt: pretty-print Typed Tree and exit.
} CliArgs;

/**
 * Parse argv into a CliArgs struct.  Fatally exits on unrecognised flags,
 * missing arguments, or absent input file.
 */
static CliArgs parse_cli_args(int argc, char *argv[]) {
    CliArgs args = {0};
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage();
        } else if (strcmp(argv[i], "--dump-tokens") == 0) {
            args.dump_tokens = true;
        } else if (strcmp(argv[i], "--dump-ast") == 0) {
            args.dump_ast = true;
        } else if (strcmp(argv[i], "--dump-tt") == 0) {
            args.dump_tt = true;
        } else if (strcmp(argv[i], "-o") == 0) {
            if (++i >= argc) {
                rsg_fatal("-o requires an argument");
            }
            args.output_file = argv[i];
        } else if (argv[i][0] == '-') {
            rsg_fatal("unknown option '%s'", argv[i]);
        } else {
            if (args.input_file != NULL) {
                rsg_fatal("multiple input files not supported");
            }
            args.input_file = argv[i];
        }
    }
    if (args.input_file == NULL) {
        fprintf(stderr, "error: no input file\n");
        usage();
    }
    return args;
}

/** Run the lexer and optionally dump tokens.  Returns 0 on success, 1 on error. */
static int run_lexer(const CliArgs *args, const char *source, Arena *arena, Token **out_tokens) {
    Lexer *lexer = lexer_create(source, args->input_file, arena);
    *out_tokens = lexer_scan_all(lexer);
    lexer_destroy(lexer);

    for (int32_t i = 0; i < BUFFER_LENGTH(*out_tokens); i++) {
        if ((*out_tokens)[i].kind == TOKEN_ERROR) {
            return 1;
        }
    }

    if (args->dump_tokens) {
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
static ASTNode *run_parser(const CliArgs *args, Token *tokens, int32_t count, Arena *arena) {
    Parser *parser = parser_create(tokens, count, arena, args->input_file);
    ASTNode *file_node = parser_parse(parser);
    parser_destroy(parser);

    if (args->dump_ast) {
        ast_dump(file_node, 0);
        return NULL;
    }
    return file_node;
}

/** Run semantic analysis.  Returns true on success, false on errors. */
static bool run_sema(Arena *arena, ASTNode *file_node) {
    SemanticAnalyzer *analyzer = semantic_analyzer_create(arena);
    bool ok = semantic_analyzer_check(analyzer, file_node);
    semantic_analyzer_destroy(analyzer);
    return ok;
}

/** Lower the AST to typed tree, run TT passes, and optionally dump.  Returns NULL on early exit. */
static TtNode *run_lowering(const CliArgs *args, Arena *tt_arena, Arena *arena, ASTNode *file_node,
                            Lowering **out_lowering) {
    *out_lowering = lowering_create(tt_arena, arena);
    TtNode *tt_root = lowering_lower(*out_lowering, file_node);

    tt_pass_const_fold(tt_arena, tt_root);

    if (args->dump_tt) {
        tt_dump(tt_root, 0);
        return NULL;
    }
    return tt_root;
}

/** Run code generation, emitting C source to the output file. */
static void run_codegen(const CliArgs *args, Arena *arena, const TtNode *tt_root) {
    FILE *out = stdout;
    if (args->output_file != NULL) {
        out = fopen(args->output_file, "w");
        if (out == NULL) {
            rsg_fatal("cannot open output '%s'", args->output_file);
        }
    }
    CodeGenerator *code_generator = code_generator_create(out, arena);
    code_generator_emit(code_generator, tt_root);
    code_generator_destroy(code_generator);
    if (args->output_file != NULL) {
        fclose(out);
    }
}

/**
 * Drive the full compilation pipeline: lex -> parse -> sema -> codegen.
 * Returns 0 on success or 1 when semantic analysis reports errors.
 * Debug flags in @p args may short-circuit after lexing or parsing.
 */
static int compile(const CliArgs *args) {
    char *source = read_file(args->input_file);
    Arena *arena = arena_create();
    Arena *tt_arena = arena_create();
    Token *tokens = NULL; /* buf */
    TtNode *tt_root = NULL;
    Lowering *lowering = NULL;
    int status = 0;

    // Stage 1: Lexical analysis.
    status = run_lexer(args, source, arena, &tokens);
    if (status != 0) {
        status = (status == -1) ? 0 : status;
        goto cleanup;
    }

    // Stage 2: Parsing.
    ASTNode *file_node = run_parser(args, tokens, BUFFER_LENGTH(tokens), arena);
    if (file_node == NULL) {
        goto cleanup;
    }

    // Stage 3: Semantic analysis.
    if (!run_sema(arena, file_node)) {
        status = 1;
        goto cleanup;
    }

    // Stage 4: Lowering + TT passes.
    tt_root = run_lowering(args, tt_arena, arena, file_node, &lowering);
    if (tt_root == NULL) {
        goto cleanup;
    }

    // Stage 5: Code generation.
    run_codegen(args, arena, tt_root);

cleanup:
    lowering_destroy(lowering);
    BUFFER_FREE(tokens);
    free(source);
    source = NULL;
    arena_destroy(tt_arena);
    arena_destroy(arena);
    return status;
}

/** Entry point: parse CLI flags, then run the compilation pipeline. */
int main(int argc, char *argv[]) {
    CliArgs args = parse_cli_args(argc, argv);
    return compile(&args);
}
