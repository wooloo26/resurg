#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lsp_server.h"

/**
 * @file lsp_main.c
 * @brief Entry point for the rsg-lsp language server.
 *
 * Communicates over stdin/stdout using the Language Server Protocol
 * with JSON-RPC transport (Content-Length framing).
 */

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char *argv[]) {
#ifdef _WIN32
    // Set stdin/stdout to binary mode to prevent CR/LF translation.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    const char *std_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--std-path=", 11) == 0) {
            std_path = argv[i] + 11;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fprintf(stderr, "Usage: rsg-lsp [--std-path=DIR]\n"
                            "\n"
                            "Resurg language server (LSP over stdin/stdout).\n");
            return 0;
        } else if (strcmp(argv[i], "--version") == 0) {
            fprintf(stderr, "rsg-lsp 1.0.0\n");
            return 0;
        }
    }

    LspServer *srv = lsp_server_create(stdin, stdout);
    if (srv == NULL) {
        fprintf(stderr, "fatal: failed to create LSP server\n");
        return 1;
    }

    if (std_path != NULL) {
        lsp_server_set_std_path(srv, std_path);
    }

    int status = lsp_server_run(srv);
    lsp_server_destroy(srv);
    return status;
}
