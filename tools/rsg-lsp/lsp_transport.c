#include "lsp_transport.h"

#include <stdlib.h>
#include <string.h>

/**
 * @file lsp_transport.c
 * @brief JSON-RPC framing: read/write Content-Length-delimited messages.
 */

char *lsp_transport_read(FILE *in) {
    // Read headers until empty line (\r\n\r\n).
    int content_length = -1;
    char header_line[512];

    for (;;) {
        if (fgets(header_line, (int)sizeof(header_line), in) == NULL) {
            return NULL; // EOF or error
        }
        // Empty line (just \r\n) terminates headers.
        if (strcmp(header_line, "\r\n") == 0 || strcmp(header_line, "\n") == 0) {
            break;
        }
        // Parse Content-Length.
        if (strncmp(header_line, "Content-Length:", 15) == 0) {
            content_length = atoi(header_line + 15);
        }
    }

    if (content_length <= 0) {
        return NULL;
    }

    // Read exactly content_length bytes.
    char *body = malloc((size_t)content_length + 1);
    if (body == NULL) {
        return NULL;
    }
    size_t n = fread(body, 1, (size_t)content_length, in);
    if (n != (size_t)content_length) {
        free(body);
        return NULL;
    }
    body[content_length] = '\0';
    return body;
}

void lsp_transport_write(FILE *out, const char *body, size_t len) {
    fprintf(out, "Content-Length: %zu\r\n\r\n", len);
    fwrite(body, 1, len, out);
    fflush(out);
}
