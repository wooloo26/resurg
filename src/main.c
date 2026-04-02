#include "codegen.h"
#include "common.h"
#include "lexer.h"
#include "parser.h"
#include "sema.h"

// ------------------------------------------------------------------------
// File I/O
// ------------------------------------------------------------------------
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        rg_fatal("cannot open '%s'", path);
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc(size + 1);
    if (buf == NULL) {
        rg_fatal("out of memory");
    }
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);
    return buf;
}

// ------------------------------------------------------------------------
// Usage
// ------------------------------------------------------------------------
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

// ------------------------------------------------------------------------
// CLI argument parsing
// ------------------------------------------------------------------------
typedef struct {
    const char *input_file;
    const char *output_file;
    bool dump_tokens;
    bool dump_ast;
} CliArgs;

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
                rg_fatal("-o requires an argument");
            }
            args.output_file = argv[i];
        } else if (argv[i][0] == '-') {
            rg_fatal("unknown option '%s'", argv[i]);
        } else {
            if (args.input_file != NULL) {
                rg_fatal("multiple input files not supported");
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

// ------------------------------------------------------------------------
// Compilation pipeline
// ------------------------------------------------------------------------
static int compile(const CliArgs *args) {
    char *source = read_file(args->input_file);
    Arena *arena = arena_create();
    Token *tokens = NULL;
    Lexer *lexer = NULL;
    Parser *parser = NULL;
    Sema *sema = NULL;
    CodeGen *cg = NULL;
    int status = 0;

    lexer = lexer_create(source, args->input_file, arena);
    tokens = lexer_scan_all(lexer);

    if (args->dump_tokens) {
        for (int32_t i = 0; i < BUF_LEN(tokens); i++) {
            Token *t = &tokens[i];
            fprintf(stderr, "%3d:%-3d  %-16s  '%.*s'\n", t->loc.line, t->loc.col, token_kind_str(t->kind), t->length,
                    t->lexeme);
        }
        goto cleanup;
    }

    parser = parser_create(tokens, BUF_LEN(tokens), arena, args->input_file);
    ASTNode *file_node = parser_parse(parser);

    if (args->dump_ast) {
        ast_dump(file_node, 0);
        goto cleanup;
    }

    sema = sema_create(arena);
    if (!sema_check(sema, file_node)) {
        status = 1;
        goto cleanup;
    }

    {
        FILE *out = stdout;
        if (args->output_file != NULL) {
            out = fopen(args->output_file, "w");
            if (out == NULL) {
                rg_fatal("cannot open output '%s'", args->output_file);
            }
        }
        cg = codegen_create(out, arena);
        codegen_emit(cg, file_node);
        if (args->output_file != NULL) {
            fclose(out);
        }
    }

cleanup:
    codegen_destroy(cg);
    sema_destroy(sema);
    parser_destroy(parser);
    lexer_destroy(lexer);
    BUF_FREE(tokens);
    free(source);
    source = NULL;
    arena_destroy(arena);
    return status;
}

// ------------------------------------------------------------------------
// Main
// ------------------------------------------------------------------------
int main(int argc, char *argv[]) {
    CliArgs args = parse_cli_args(argc, argv);
    return compile(&args);
}
