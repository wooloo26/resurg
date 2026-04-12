#ifndef RSG_LSP_TRANSPORT_H
#define RSG_LSP_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/**
 * @file lsp_transport.h
 * @brief JSON-RPC transport over stdio with LSP Content-Length framing.
 */

/** Read one JSON-RPC message body from @p in. Returns heap-allocated string, or NULL on EOF/error.
 */
char *lsp_transport_read(FILE *in);

/** Write a JSON-RPC message with Content-Length header to @p out. */
void lsp_transport_write(FILE *out, const char *body, size_t len);

#endif // RSG_LSP_TRANSPORT_H
