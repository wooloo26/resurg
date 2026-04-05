#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>

#include "rsg/compiler.h"

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

/**
 * Parse argv into a CompilerOptions struct.  Fatally exits on unrecognised
 * flags, missing arguments, or absent input file.
 */
static CompilerOptions parse_cli_args(int argc, char *argv[]) {
    CompilerOptions options = {0};
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage();
        } else if (strcmp(argv[i], "--dump-tokens") == 0) {
            options.dump_tokens = true;
        } else if (strcmp(argv[i], "--dump-ast") == 0) {
            options.dump_ast = true;
        } else if (strcmp(argv[i], "--dump-tt") == 0) {
            options.dump_tt = true;
        } else if (strcmp(argv[i], "-o") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "error: -o requires an argument\n");
                usage();
            }
            options.output_file = argv[i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
            usage();
        } else {
            if (options.input_file != NULL) {
                fprintf(stderr, "error: multiple input files not supported\n");
                usage();
            }
            options.input_file = argv[i];
        }
    }
    if (options.input_file == NULL) {
        fprintf(stderr, "error: no input file\n");
        usage();
    }
    return options;
}

/** Entry point: parse CLI flags, then run the compilation pipeline. */
int main(int argc, char *argv[]) {
    CompilerOptions options = parse_cli_args(argc, argv);
    Compiler *compiler = compiler_create();
    int status = compiler_run(compiler, &options);
    compiler_destroy(compiler);
    return status;
}
