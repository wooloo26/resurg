#include <sys/stat.h>

#include "codegen.h"
#include "common.h"
#include "lexer.h"
#include "parser.h"
#include "sema.h"

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

    fseek(file_handle, 0, SEEK_END);
    long size = ftell(file_handle);
    if (size < 0 || size > MAX_SOURCE_SIZE) {
        fclose(file_handle);
        rsg_fatal("cannot read '%s' (size error or file too large)", path);
    }
    fseek(file_handle, 0, SEEK_SET);

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

/**
 * Drive the full compilation pipeline: lex -> parse -> sema -> codegen.
 * Returns 0 on success or 1 when semantic analysis reports errors.
 * Debug flags in @p args may short-circuit after lexing or parsing.
 */
static int compile(const CliArgs *args) {
    char *source = read_file(args->input_file);
    Arena *arena = arena_create();
    Token *tokens = NULL; /* buf */
    Lexer *lexer = NULL;
    Parser *parser = NULL;
    SemanticAnalyzer *analyzer = NULL;
    CodeGenerator *code_generator = NULL;
    int status = 0;

    // Stage 1: Lexical analysis.
    lexer = lexer_create(source, args->input_file, arena);
    tokens = lexer_scan_all(lexer);

    // Bail out early if the lexer produced any error tokens.
    for (int32_t i = 0; i < BUFFER_LENGTH(tokens); i++) {
        if (tokens[i].kind == TOKEN_ERROR) {
            status = 1;
            break;
        }
    }
    if (status != 0) {
        goto cleanup;
    }

    if (args->dump_tokens) {
        for (int32_t i = 0; i < BUFFER_LENGTH(tokens); i++) {
            Token *token = &tokens[i];
            int32_t line = token->location.line;
            int32_t column = token->location.column;
            const char *kind = token_kind_string(token->kind);
            fprintf(stderr, "%3d:%-3d  %-16s  '%.*s'\n", line, column, kind, token->length,
                    token->lexeme);
        }
        goto cleanup;
    }

    // Stage 2: Parsing - build the AST from the token stream.
    parser = parser_create(tokens, BUFFER_LENGTH(tokens), arena, args->input_file);
    ASTNode *file_node = parser_parse(parser);

    if (args->dump_ast) {
        ast_dump(file_node, 0);
        goto cleanup;
    }

    // Stage 3: Semantic analysis - type-check and validate the AST.
    analyzer = semantic_analyzer_create(arena);
    if (!semantic_analyzer_check(analyzer, file_node)) {
        status = 1;
        goto cleanup;
    }

    // Stage 4: Code generation - emit C source from the validated AST.
    {
        FILE *out = stdout;
        if (args->output_file != NULL) {
            out = fopen(args->output_file, "w");
            if (out == NULL) {
                rsg_fatal("cannot open output '%s'", args->output_file);
            }
        }
        code_generator = code_generator_create(out, arena);
        code_generator_emit(code_generator, file_node);
        if (args->output_file != NULL) {
            fclose(out);
        }
    }

cleanup:
    code_generator_destroy(code_generator);
    semantic_analyzer_destroy(analyzer);
    parser_destroy(parser);
    lexer_destroy(lexer);
    BUFFER_FREE(tokens);
    free(source);
    source = NULL;
    arena_destroy(arena);
    return status;
}

/** Entry point: parse CLI flags, then run the compilation pipeline. */
int main(int argc, char *argv[]) {
    CliArgs args = parse_cli_args(argc, argv);
    return compile(&args);
}
