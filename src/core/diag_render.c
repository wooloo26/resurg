#include "core/diag.h"

/**
 * @file diag_render.c
 * @brief Diagnostic rendering — text, source snippets, and JSON output.
 *
 * Separated from diag.c (emission/storage) so that rendering concerns
 * (source line reading, formatting, JSON encoding) do not pollute the
 * core diagnostic collection API.
 */

// ── Level label ────────────────────────────────────────────────────────

const char *diag_level_str(DiagLevel level) {
    switch (level) {
    case DIAG_ERR:
        return "err";
    case DIAG_WARN:
        return "warning";
    case DIAG_NOTE:
        return "note";
    case DIAG_HELP:
        return "help";
    }
    return "unknown";
}

// ── Source line reading ────────────────────────────────────────────────

/**
 * Read a single source line from @p path at 1-based @p line_num.
 * Returns a heap-allocated string (caller frees), or NULL on failure.
 */
static char *read_source_line(const char *path, int32_t line_num) {
    if (path == NULL || line_num <= 0) {
        return NULL;
    }
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return NULL;
    }
    char line_buf[1024];
    int32_t current = 1;
    while (fgets(line_buf, (int)sizeof(line_buf), f) != NULL) {
        if (current == line_num) {
            fclose(f);
            // Strip trailing newline.
            size_t len = strlen(line_buf);
            if (len > 0 && line_buf[len - 1] == '\n') {
                line_buf[len - 1] = '\0';
                len--;
            }
            if (len > 0 && line_buf[len - 1] == '\r') {
                line_buf[len - 1] = '\0';
                len--;
            }
            char *result = rsg_malloc(len + 1);
            memcpy(result, line_buf, len + 1);
            return result;
        }
        current++;
    }
    fclose(f);
    return NULL;
}

// ── Text rendering ─────────────────────────────────────────────────────

void diag_render_simple(const Diagnostic *diag, FILE *out) {
    if (diag->loc.file != NULL) {
        fprintf(out, "%s:%d:%d: ", diag->loc.file, diag->loc.line, diag->loc.column);
    }
    fprintf(out, "%s", diag_level_str(diag->level));
    if (diag->code != NULL) {
        fprintf(out, "[%s]", diag->code);
    }
    fprintf(out, ": %s\n", diag->message);
}

/** Render a single diagnostic with optional source snippet. */
static void diag_render_one(const Diagnostic *diag, FILE *out) {
    // Header line: file:line:col: level[CODE]: message
    if (diag->loc.file != NULL) {
        fprintf(out, "%s:%d:%d: ", diag->loc.file, diag->loc.line, diag->loc.column);
    }
    fprintf(out, "%s", diag_level_str(diag->level));
    if (diag->code != NULL) {
        fprintf(out, "[%s]", diag->code);
    }
    fprintf(out, ": %s\n", diag->message);

    // Source snippet (when file is available).
    char *source_line = read_source_line(diag->loc.file, diag->loc.line);
    if (source_line != NULL) {
        int32_t line_num = diag->loc.line;
        int32_t col = diag->loc.column;

        // Compute underline width.
        int32_t span_len = 1;
        if (diag->end_loc.line == diag->loc.line && diag->end_loc.column > diag->loc.column) {
            span_len = diag->end_loc.column - diag->loc.column;
        }

        // Compute gutter width for line number.
        int32_t gutter_width = 1;
        {
            int32_t tmp = line_num;
            while (tmp >= 10) {
                tmp /= 10;
                gutter_width++;
            }
        }

        // " --> file:line:col"
        fprintf(out, " %*s--> %s:%d:%d\n", gutter_width, "", diag->loc.file, line_num, col);
        // " | "
        fprintf(out, " %*s |\n", gutter_width, "");
        // "NN | source line"
        fprintf(out, " %d | %s\n", line_num, source_line);
        // "   | ^^^^ "
        fprintf(out, " %*s | ", gutter_width, "");
        for (int32_t i = 1; i < col; i++) {
            fputc(' ', out);
        }
        for (int32_t i = 0; i < span_len; i++) {
            fputc('^', out);
        }
        fputc('\n', out);
        // " | "
        fprintf(out, " %*s |\n", gutter_width, "");

        free(source_line);
    }

    // Render attached notes/help.
    for (int32_t i = 0; i < BUF_LEN(diag->related); i++) {
        const Diagnostic *rel = &diag->related[i];
        fprintf(out, "       = %s: %s\n", diag_level_str(rel->level), rel->message);
    }
}

void diag_render_all(const DiagCtx *dctx, FILE *out) {
    for (int32_t i = 0; i < BUF_LEN(dctx->diags); i++) {
        diag_render_one(&dctx->diags[i], out);
    }
}

// ── JSON rendering ─────────────────────────────────────────────────────

/** Map DiagLevel to an LSP-compatible severity int (1=Error, 2=Warning, 3=Info, 4=Hint). */
static int diag_level_to_lsp_severity(DiagLevel level) {
    switch (level) {
    case DIAG_ERR:
        return 1;
    case DIAG_WARN:
        return 2;
    case DIAG_NOTE:
        return 3;
    case DIAG_HELP:
        return 4;
    }
    return 1;
}

/** Write a JSON-escaped version of @p str to @p out. */
static void json_write_escaped_str(FILE *out, const char *str) {
    fputc('"', out);
    if (str == NULL) {
        fputc('"', out);
        return;
    }
    for (const char *p = str; *p != '\0'; p++) {
        switch (*p) {
        case '"':
            fputs("\\\"", out);
            break;
        case '\\':
            fputs("\\\\", out);
            break;
        case '\n':
            fputs("\\n", out);
            break;
        case '\r':
            fputs("\\r", out);
            break;
        case '\t':
            fputs("\\t", out);
            break;
        default:
            if ((unsigned char)*p < 0x20) {
                fprintf(out, "\\u%04x", (unsigned char)*p);
            } else {
                fputc(*p, out);
            }
            break;
        }
    }
    fputc('"', out);
}

/** Render a single diagnostic as a JSON object. */
static void diag_render_one_json(const Diagnostic *diag, FILE *out) {
    fprintf(out, "{\"severity\":%d", diag_level_to_lsp_severity(diag->level));

    fputs(",\"file\":", out);
    json_write_escaped_str(out, diag->loc.file);

    fprintf(out, ",\"line\":%d,\"column\":%d", diag->loc.line, diag->loc.column);
    fprintf(out, ",\"endLine\":%d,\"endColumn\":%d", diag->end_loc.line, diag->end_loc.column);

    fputs(",\"code\":", out);
    if (diag->code != NULL) {
        json_write_escaped_str(out, diag->code);
    } else {
        fputs("null", out);
    }

    fputs(",\"message\":", out);
    json_write_escaped_str(out, diag->message);

    // Related diagnostics (notes, help).
    fputs(",\"related\":[", out);
    for (int32_t i = 0; i < BUF_LEN(diag->related); i++) {
        if (i > 0) {
            fputc(',', out);
        }
        diag_render_one_json(&diag->related[i], out);
    }
    fputs("]}", out);
}

void diag_render_json(const DiagCtx *dctx, FILE *out) {
    fputc('[', out);
    for (int32_t i = 0; i < BUF_LEN(dctx->diags); i++) {
        if (i > 0) {
            fputc(',', out);
        }
        diag_render_one_json(&dctx->diags[i], out);
    }
    fputs("]\n", out);
}
