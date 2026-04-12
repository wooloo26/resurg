#ifndef RSG_LSP_SERVER_H
#define RSG_LSP_SERVER_H

#include <stdbool.h>
#include <stdio.h>

/**
 * @file lsp_server.h
 * @brief LSP server — method dispatch and state management.
 */

typedef struct LspServer LspServer;

/** Create a new LSP server that reads from @p in and writes to @p out. */
LspServer *lsp_server_create(FILE *in, FILE *out);

/** Set the std library path for compilation. */
void lsp_server_set_std_path(LspServer *srv, const char *std_path);

/** Run the server message loop until exit. Returns the exit code. */
int lsp_server_run(LspServer *srv);

/** Destroy the server and free resources. */
void lsp_server_destroy(LspServer *srv);

#endif // RSG_LSP_SERVER_H
