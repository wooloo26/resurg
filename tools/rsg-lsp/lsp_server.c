#include "lsp_server.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef S_ISREG
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif

#ifdef _WIN32
#define PATH_SEP '\\'
#else
#define PATH_SEP '/'
#endif

#include "core/common.h"
#include "core/diag.h"
#include "lsp_json.h"
#include "lsp_semtokens.h"
#include "lsp_symbol.h"
#include "lsp_transport.h"
#include "pass/lex/lex.h"
#include "repr/ast.h"
#include "rsg/driver/pipeline.h"
#include "rsg/pass/check/check.h"
#include "rsg/pass/mono/mono.h"
#include "rsg/pass/parse/parse.h"
#include "rsg/pass/resolve/resolve.h"

/**
 * @file lsp_server.c
 * @brief LSP server — handles initialize, didOpen, didChange, shutdown, exit.
 *
 * Uses the composable pipeline API to lex, parse, and check .rsg files,
 * then publishes diagnostics in LSP format.
 */

// ── Portable strdup ────────────────────────────────────────────────────

static char *lsp_strdup(const char *s) {
    if (s == NULL) {
        return NULL;
    }
    size_t len = strlen(s);
    char *copy = malloc(len + 1);
    if (copy != NULL) {
        memcpy(copy, s, len + 1);
    }
    return copy;
}

// ── File reading ───────────────────────────────────────────────────────

/** Maximum source file size (64 MiB). */
#define MAX_SRC_SIZE (64L * 1024 * 1024)

/** Read entire file into heap-allocated NUL-terminated buffer. */
static char *lsp_read_file(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return NULL;
    }
    long size = (long)st.st_size;
    if (size < 0 || size > MAX_SRC_SIZE) {
        return NULL;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    char *buf = calloc((size_t)size + 1, 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (n != (size_t)size) {
        free(buf);
        return NULL;
    }
    return buf;
}

/**
 * Parse an .rsg file and prepend its declarations to @p file_node.
 * Returns true when at least one declaration was prepended.
 */
static bool lsp_prepend_rsg_file(Arena *arena, const char *path, ASTNode *file_node) {
    char *src = lsp_read_file(path);
    if (src == NULL) {
        return false;
    }

    Lex *lex = lex_create(src, path, arena, NULL);
    Token *tokens = lex_scan_all(lex);
    lex_destroy(lex);

    int32_t count = BUF_LEN(tokens);
    Parser *parser = parser_create(tokens, count, arena, path, NULL);
    ASTNode *parsed = parser_parse(parser);
    int32_t errs = parser_err_count(parser);
    parser_destroy(parser);
    free(src);

    if (errs > 0 || parsed == NULL) {
        return false;
    }

    int32_t parsed_count = BUF_LEN(parsed->file.decls);
    if (parsed_count == 0) {
        return false;
    }

    int32_t user_count = BUF_LEN(file_node->file.decls);
    ASTNode **merged = NULL;
    for (int32_t i = 0; i < parsed_count; i++) {
        BUF_PUSH(merged, parsed->file.decls[i]);
    }
    for (int32_t i = 0; i < user_count; i++) {
        BUF_PUSH(merged, file_node->file.decls[i]);
    }

    BUF_FREE(file_node->file.decls);
    file_node->file.decls = merged;
    return true;
}

/** Module loader callback: lex+parse a module file, return its decls. */
static ASTNode **lsp_load_module(void *ctx, Arena *arena, const char *mod_path) {
    (void)ctx;
    char *src = lsp_read_file(mod_path);
    if (src == NULL) {
        return NULL;
    }
    Lex *lex = lex_create(src, mod_path, arena, NULL);
    Token *tokens = lex_scan_all(lex);
    lex_destroy(lex);

    int32_t count = BUF_LEN(tokens);
    Parser *parser = parser_create(tokens, count, arena, mod_path, NULL);
    ASTNode *file_node = parser_parse(parser);
    int32_t errs = parser_err_count(parser);
    parser_destroy(parser);
    free(src);

    if (errs > 0 || file_node == NULL) {
        return NULL;
    }
    return file_node->file.decls;
}

// ── Document store ─────────────────────────────────────────────────────

/** An open document tracked by the server. */
typedef struct LspDocument {
    char *uri;
    char *text;
    int32_t version;
    LspSymEntry *symbols; // buf - symbol index (rebuilt on each change)
    SemToken *sem_tokens; // buf - semantic tokens (rebuilt on each change)
    struct LspDocument *next;
} LspDocument;

struct LspServer {
    FILE *in;
    FILE *out;
    bool initialized;
    bool shutdown_requested;
    const char *std_path;
    LspDocument *documents; // linked list of open documents
};

// ── Helpers ────────────────────────────────────────────────────────────

/** Convert a file:// URI to a filesystem path (heap-allocated). */
static char *uri_to_path(const char *uri) {
    if (uri == NULL) {
        return NULL;
    }
    const char *path = uri;
    // NOLINTBEGIN(bugprone-branch-clone) - intentional: Windows uses +8, Unix uses +7
    if (strncmp(uri, "file:///", 8) == 0) {
#ifdef _WIN32
        // file:///C:/foo → C:/foo
        path = uri + 8;
#else
        // file:///foo → /foo
        path = uri + 7;
#endif
    } else if (strncmp(uri, "file://", 7) == 0) {
        path = uri + 7;
    }
    // NOLINTEND(bugprone-branch-clone)
    size_t len = strlen(path);
    char *result = malloc(len + 1);
    if (result == NULL) {
        return NULL;
    }
    // Decode percent-encoded characters.
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (path[i] == '%' && i + 2 < len) {
            char hex[3] = {path[i + 1], path[i + 2], '\0'};
            result[j++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            result[j++] = path[i];
        }
    }
    result[j] = '\0';
    return result;
}

static LspDocument *find_document(LspServer *srv, const char *uri) {
    for (LspDocument *doc = srv->documents; doc != NULL; doc = doc->next) {
        if (strcmp(doc->uri, uri) == 0) {
            return doc;
        }
    }
    return NULL;
}

static void free_symbol_index(LspSymEntry *syms) {
    int32_t n = BUF_LEN(syms);
    for (int32_t i = 0; i < n; i++) {
        lsp_sym_entry_free(&syms[i]);
    }
    BUF_FREE(syms);
}

static LspDocument *upsert_document(LspServer *srv, const char *uri, const char *text,
                                    int32_t version) {
    LspDocument *doc = find_document(srv, uri);
    if (doc != NULL) {
        free(doc->text);
        doc->text = lsp_strdup(text);
        doc->version = version;
        free_symbol_index(doc->symbols);
        doc->symbols = NULL;
        BUF_FREE(doc->sem_tokens);
        doc->sem_tokens = NULL;
        return doc;
    }
    doc = calloc(1, sizeof(LspDocument));
    if (doc == NULL) {
        return NULL;
    }
    doc->uri = lsp_strdup(uri);
    doc->text = lsp_strdup(text);
    doc->version = version;
    doc->next = srv->documents;
    srv->documents = doc;
    return doc;
}

static void remove_document(LspServer *srv, const char *uri) {
    LspDocument **pp = &srv->documents;
    while (*pp != NULL) {
        if (strcmp((*pp)->uri, uri) == 0) {
            LspDocument *old = *pp;
            *pp = old->next;
            free(old->uri);
            free(old->text);
            free_symbol_index(old->symbols);
            BUF_FREE(old->sem_tokens);
            free(old);
            return;
        }
        pp = &(*pp)->next;
    }
}

// ── Response helpers ───────────────────────────────────────────────────

static void send_response(LspServer *srv, int64_t id, const char *result_json) {
    JsonBuf b;
    jbuf_init(&b);
    jbuf_obj_start(&b);
    jbuf_key(&b, "jsonrpc");
    jbuf_str_val(&b, "2.0");
    jbuf_comma(&b);
    jbuf_key(&b, "id");
    jbuf_int_val(&b, id);
    jbuf_comma(&b);
    jbuf_key(&b, "result");
    jbuf_raw(&b, result_json);
    jbuf_obj_end(&b);
    lsp_transport_write(srv->out, jbuf_str(&b), b.len);
    jbuf_destroy(&b);
}

static void send_notification(LspServer *srv, const char *method, const char *params_json) {
    JsonBuf b;
    jbuf_init(&b);
    jbuf_obj_start(&b);
    jbuf_key(&b, "jsonrpc");
    jbuf_str_val(&b, "2.0");
    jbuf_comma(&b);
    jbuf_key(&b, "method");
    jbuf_str_val(&b, method);
    jbuf_comma(&b);
    jbuf_key(&b, "params");
    jbuf_raw(&b, params_json);
    jbuf_obj_end(&b);
    lsp_transport_write(srv->out, jbuf_str(&b), b.len);
    jbuf_destroy(&b);
}

// ── Diagnostics ────────────────────────────────────────────────────────

/** Map DiagLevel to LSP severity int (1=Error, 2=Warning, 3=Info, 4=Hint). */
static int diag_level_to_severity(DiagLevel level) {
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

/**
 * Run lex → parse → resolve → check on a document and collect diagnostics.
 * Produces an LSP-format JSON diagnostics array string (heap-allocated).
 * If @p out_symbols is non-NULL, builds a symbol index (caller must BUF_FREE).
 * If @p out_sem_tokens is non-NULL, builds semantic tokens (caller must BUF_FREE).
 */
static char *compile_diagnostics(LspServer *srv, const char *text, const char *file_path,
                                 LspSymEntry **out_symbols, SemToken **out_sem_tokens) {
    if (out_symbols != NULL) {
        *out_symbols = NULL;
    }
    if (out_sem_tokens != NULL) {
        *out_sem_tokens = NULL;
    }
    Pipeline *pipeline = pipeline_create();
    Arena *arena = arena_create();
    // Store arena into pipeline internals for composable API.
    // We need to manually manage the pipeline struct internals.
    // Instead, use a local pipeline_run approach.

    // Lex.
    Lex *lex = lex_create(text, file_path, arena, NULL);
    Token *tokens = lex_scan_all(lex);
    lex_destroy(lex);

    JsonBuf diag_buf;
    jbuf_init(&diag_buf);
    jbuf_arr_start(&diag_buf);

    bool has_lex_errors = false;
    for (int32_t i = 0; i < BUF_LEN(tokens); i++) {
        if (tokens[i].kind == TOKEN_ERR) {
            has_lex_errors = true;
            // Emit lex error as diagnostic.
            int32_t line = tokens[i].loc.line > 0 ? tokens[i].loc.line - 1 : 0;
            int32_t col = tokens[i].loc.column > 0 ? tokens[i].loc.column - 1 : 0;

            if (diag_buf.len > 1 && diag_buf.data[diag_buf.len - 1] != '[') {
                jbuf_comma(&diag_buf);
            }

            jbuf_obj_start(&diag_buf);
            jbuf_key(&diag_buf, "range");
            jbuf_obj_start(&diag_buf);
            jbuf_key(&diag_buf, "start");
            jbuf_obj_start(&diag_buf);
            jbuf_key(&diag_buf, "line");
            jbuf_int_val(&diag_buf, line);
            jbuf_comma(&diag_buf);
            jbuf_key(&diag_buf, "character");
            jbuf_int_val(&diag_buf, col);
            jbuf_obj_end(&diag_buf);
            jbuf_comma(&diag_buf);
            jbuf_key(&diag_buf, "end");
            jbuf_obj_start(&diag_buf);
            jbuf_key(&diag_buf, "line");
            jbuf_int_val(&diag_buf, line);
            jbuf_comma(&diag_buf);
            jbuf_key(&diag_buf, "character");
            jbuf_int_val(&diag_buf, col + tokens[i].len);
            jbuf_obj_end(&diag_buf);
            jbuf_obj_end(&diag_buf);
            jbuf_comma(&diag_buf);
            jbuf_key(&diag_buf, "severity");
            jbuf_int_val(&diag_buf, 1);
            jbuf_comma(&diag_buf);
            jbuf_key(&diag_buf, "source");
            jbuf_str_val(&diag_buf, "resurg");
            jbuf_comma(&diag_buf);
            jbuf_key(&diag_buf, "message");
            jbuf_str_val(&diag_buf, tokens[i].lexeme != NULL ? tokens[i].lexeme : "lex error");
            jbuf_obj_end(&diag_buf);
        }
    }

    if (has_lex_errors) {
        goto done;
    }

    // Parse.
    {
        DiagCtx parse_dctx;
        diag_ctx_init(&parse_dctx, arena);
        int32_t count = BUF_LEN(tokens);
        Parser *parser = parser_create(tokens, count, arena, file_path, &parse_dctx);
        ASTNode *file_node = parser_parse(parser);
        int32_t parse_errs = parser_err_count(parser);
        parser_destroy(parser);

        if (parse_errs > 0 || file_node == NULL) {
            // Emit captured parse diagnostics with location info.
            for (int32_t i = 0; i < BUF_LEN(parse_dctx.diags); i++) {
                Diagnostic *d = &parse_dctx.diags[i];
                if (d->level != DIAG_ERR && d->level != DIAG_WARN) {
                    continue;
                }

                int32_t line = d->loc.line > 0 ? d->loc.line - 1 : 0;
                int32_t col = d->loc.column > 0 ? d->loc.column - 1 : 0;
                int32_t end_line = d->end_loc.line > 0 ? d->end_loc.line - 1 : line;
                int32_t end_col = d->end_loc.column > 0 ? d->end_loc.column - 1 : col + 1;

                if (diag_buf.len > 1 && diag_buf.data[diag_buf.len - 1] != '[') {
                    jbuf_comma(&diag_buf);
                }

                jbuf_obj_start(&diag_buf);
                jbuf_key(&diag_buf, "range");
                jbuf_obj_start(&diag_buf);
                jbuf_key(&diag_buf, "start");
                jbuf_obj_start(&diag_buf);
                jbuf_key(&diag_buf, "line");
                jbuf_int_val(&diag_buf, line);
                jbuf_comma(&diag_buf);
                jbuf_key(&diag_buf, "character");
                jbuf_int_val(&diag_buf, col);
                jbuf_obj_end(&diag_buf);
                jbuf_comma(&diag_buf);
                jbuf_key(&diag_buf, "end");
                jbuf_obj_start(&diag_buf);
                jbuf_key(&diag_buf, "line");
                jbuf_int_val(&diag_buf, end_line);
                jbuf_comma(&diag_buf);
                jbuf_key(&diag_buf, "character");
                jbuf_int_val(&diag_buf, end_col);
                jbuf_obj_end(&diag_buf);
                jbuf_obj_end(&diag_buf);
                jbuf_comma(&diag_buf);
                jbuf_key(&diag_buf, "severity");
                jbuf_int_val(&diag_buf, diag_level_to_severity(d->level));
                jbuf_comma(&diag_buf);
                jbuf_key(&diag_buf, "source");
                jbuf_str_val(&diag_buf, "resurg");
                jbuf_comma(&diag_buf);
                jbuf_key(&diag_buf, "message");
                jbuf_str_val(&diag_buf, d->message);
                jbuf_obj_end(&diag_buf);
            }

            // Fallback if no diags were captured
            if (BUF_LEN(parse_dctx.diags) == 0) {
                if (diag_buf.len > 1 && diag_buf.data[diag_buf.len - 1] != '[') {
                    jbuf_comma(&diag_buf);
                }
                jbuf_obj_start(&diag_buf);
                jbuf_key(&diag_buf, "range");
                jbuf_obj_start(&diag_buf);
                jbuf_key(&diag_buf, "start");
                jbuf_obj_start(&diag_buf);
                jbuf_key(&diag_buf, "line");
                jbuf_int_val(&diag_buf, 0);
                jbuf_comma(&diag_buf);
                jbuf_key(&diag_buf, "character");
                jbuf_int_val(&diag_buf, 0);
                jbuf_obj_end(&diag_buf);
                jbuf_comma(&diag_buf);
                jbuf_key(&diag_buf, "end");
                jbuf_obj_start(&diag_buf);
                jbuf_key(&diag_buf, "line");
                jbuf_int_val(&diag_buf, 0);
                jbuf_comma(&diag_buf);
                jbuf_key(&diag_buf, "character");
                jbuf_int_val(&diag_buf, 0);
                jbuf_obj_end(&diag_buf);
                jbuf_obj_end(&diag_buf);
                jbuf_comma(&diag_buf);
                jbuf_key(&diag_buf, "severity");
                jbuf_int_val(&diag_buf, 1);
                jbuf_comma(&diag_buf);
                jbuf_key(&diag_buf, "source");
                jbuf_str_val(&diag_buf, "resurg");
                jbuf_comma(&diag_buf);
                jbuf_key(&diag_buf, "message");
                jbuf_str_val(&diag_buf, "parse error");
                jbuf_obj_end(&diag_buf);
            }
            diag_ctx_destroy(&parse_dctx);
            goto done;
        }

        // Inject builtin.rsg and prelude.rsg (same as pipeline does).
        if (srv->std_path != NULL) {
            char builtin_path[1024];
            char prelude_path[1024];
            snprintf(builtin_path, sizeof(builtin_path), "%s%cbuiltin.rsg", srv->std_path,
                     PATH_SEP);
            snprintf(prelude_path, sizeof(prelude_path), "%s%cprelude.rsg", srv->std_path,
                     PATH_SEP);
            lsp_prepend_rsg_file(arena, prelude_path, file_node);
            lsp_prepend_rsg_file(arena, builtin_path, file_node);
        }

        // Sema: resolve + check + mono.
        Sema *sema = sema_create(arena);
        sema_set_module_loader(sema, lsp_load_module, NULL);
        if (srv->std_path != NULL) {
            sema_set_std_search_dir(sema, srv->std_path);
        }
        sema_enable_method_checking(sema);

        bool sema_ok = sema_resolve(sema, file_node) && sema_check(sema, file_node) &&
                       sema_mono(sema, file_node, sema_check_fn_body);

        // Collect sema diagnostics, filtering out those from injected std files.
        DiagCtx *dctx = sema_diag_ctx(sema);
        for (int32_t i = 0; i < BUF_LEN(dctx->diags); i++) {
            Diagnostic *d = &dctx->diags[i];

            // Skip diagnostics from std library files.
            if (d->loc.file != NULL && strcmp(d->loc.file, file_path) != 0) {
                continue;
            }

            int32_t line = d->loc.line > 0 ? d->loc.line - 1 : 0;
            int32_t col = d->loc.column > 0 ? d->loc.column - 1 : 0;
            int32_t end_line = d->end_loc.line > 0 ? d->end_loc.line - 1 : line;
            int32_t end_col = d->end_loc.column > 0 ? d->end_loc.column - 1 : col + 1;

            if (diag_buf.len > 1 && diag_buf.data[diag_buf.len - 1] != '[') {
                jbuf_comma(&diag_buf);
            }

            jbuf_obj_start(&diag_buf);
            jbuf_key(&diag_buf, "range");
            jbuf_obj_start(&diag_buf);
            jbuf_key(&diag_buf, "start");
            jbuf_obj_start(&diag_buf);
            jbuf_key(&diag_buf, "line");
            jbuf_int_val(&diag_buf, line);
            jbuf_comma(&diag_buf);
            jbuf_key(&diag_buf, "character");
            jbuf_int_val(&diag_buf, col);
            jbuf_obj_end(&diag_buf);
            jbuf_comma(&diag_buf);
            jbuf_key(&diag_buf, "end");
            jbuf_obj_start(&diag_buf);
            jbuf_key(&diag_buf, "line");
            jbuf_int_val(&diag_buf, end_line);
            jbuf_comma(&diag_buf);
            jbuf_key(&diag_buf, "character");
            jbuf_int_val(&diag_buf, end_col);
            jbuf_obj_end(&diag_buf);
            jbuf_obj_end(&diag_buf);
            jbuf_comma(&diag_buf);
            jbuf_key(&diag_buf, "severity");
            jbuf_int_val(&diag_buf, diag_level_to_severity(d->level));
            jbuf_comma(&diag_buf);
            jbuf_key(&diag_buf, "source");
            jbuf_str_val(&diag_buf, "resurg");
            jbuf_comma(&diag_buf);
            jbuf_key(&diag_buf, "message");
            jbuf_str_val(&diag_buf, d->message);

            // Optional error code.
            if (d->code != NULL) {
                jbuf_comma(&diag_buf);
                jbuf_key(&diag_buf, "code");
                jbuf_str_val(&diag_buf, d->code);
            }

            jbuf_obj_end(&diag_buf);
        }

        (void)sema_ok;

        // Build symbol index before destroying sema.
        if (out_symbols != NULL) {
            *out_symbols = lsp_build_symbol_index(arena, file_node, sema, file_path);
        }

        // Build semantic tokens (param/receiver refs in function bodies).
        if (out_sem_tokens != NULL) {
            *out_sem_tokens = lsp_build_semantic_tokens(file_node, file_path);
        }

        sema_destroy(sema);
        diag_ctx_destroy(&parse_dctx);
    }

done:
    BUF_FREE(tokens);
    jbuf_arr_end(&diag_buf);

    // Copy result out.
    char *result = lsp_strdup(jbuf_str(&diag_buf));
    jbuf_destroy(&diag_buf);
    arena_destroy(arena);
    pipeline_destroy(pipeline);
    return result;
}

static void publish_diagnostics(LspServer *srv, const char *uri, const char *diag_json) {
    JsonBuf b;
    jbuf_init(&b);
    jbuf_obj_start(&b);
    jbuf_key(&b, "uri");
    jbuf_str_val(&b, uri);
    jbuf_comma(&b);
    jbuf_key(&b, "diagnostics");
    jbuf_raw(&b, diag_json);
    jbuf_obj_end(&b);
    send_notification(srv, "textDocument/publishDiagnostics", jbuf_str(&b));
    jbuf_destroy(&b);
}

static void validate_document(LspServer *srv, const char *uri, const char *text) {
    char *file_path = uri_to_path(uri);
    LspSymEntry *syms = NULL;
    SemToken *sem_tokens = NULL;
    char *diag_json = compile_diagnostics(srv, text, file_path != NULL ? file_path : "buffer.rsg",
                                          &syms, &sem_tokens);
    publish_diagnostics(srv, uri, diag_json);

    // Store the symbol index and semantic tokens in the document.
    LspDocument *doc = find_document(srv, uri);
    if (doc != NULL) {
        free_symbol_index(doc->symbols);
        doc->symbols = syms;
        BUF_FREE(doc->sem_tokens);
        doc->sem_tokens = sem_tokens;
    } else {
        free_symbol_index(syms);
        BUF_FREE(sem_tokens);
    }

    free(diag_json);
    free(file_path);
}

// ── Handler dispatch ───────────────────────────────────────────────────

static void handle_initialize(LspServer *srv, int64_t id) {
    srv->initialized = true;

    // Build capabilities.
    JsonBuf cap;
    jbuf_init(&cap);
    jbuf_obj_start(&cap);

    // capabilities
    jbuf_key(&cap, "capabilities");
    jbuf_obj_start(&cap);

    // textDocumentSync: full (1) — send entire document on change.
    jbuf_key(&cap, "textDocumentSync");
    jbuf_obj_start(&cap);
    jbuf_key(&cap, "openClose");
    jbuf_bool_val(&cap, true);
    jbuf_comma(&cap);
    jbuf_key(&cap, "change");
    jbuf_int_val(&cap, 1); // TextDocumentSyncKind.Full
    jbuf_comma(&cap);
    jbuf_key(&cap, "save");
    jbuf_obj_start(&cap);
    jbuf_key(&cap, "includeText");
    jbuf_bool_val(&cap, true);
    jbuf_obj_end(&cap);
    jbuf_obj_end(&cap);

    // definitionProvider
    jbuf_comma(&cap);
    jbuf_key(&cap, "definitionProvider");
    jbuf_bool_val(&cap, true);

    // hoverProvider
    jbuf_comma(&cap);
    jbuf_key(&cap, "hoverProvider");
    jbuf_bool_val(&cap, true);

    // referencesProvider
    jbuf_comma(&cap);
    jbuf_key(&cap, "referencesProvider");
    jbuf_bool_val(&cap, true);

    // documentSymbolProvider
    jbuf_comma(&cap);
    jbuf_key(&cap, "documentSymbolProvider");
    jbuf_bool_val(&cap, true);

    // semanticTokensProvider
    jbuf_comma(&cap);
    jbuf_key(&cap, "semanticTokensProvider");
    jbuf_obj_start(&cap);
    jbuf_key(&cap, "legend");
    jbuf_obj_start(&cap);
    jbuf_key(&cap, "tokenTypes");
    jbuf_raw(&cap, "[\"namespace\",\"type\",\"class\",\"enum\",\"interface\","
                   "\"struct\",\"typeParameter\",\"parameter\",\"variable\","
                   "\"property\",\"enumMember\",\"event\",\"function\","
                   "\"method\",\"macro\",\"keyword\",\"modifier\","
                   "\"comment\",\"string\",\"number\",\"regexp\",\"operator\"]");
    jbuf_comma(&cap);
    jbuf_key(&cap, "tokenModifiers");
    jbuf_raw(&cap, "[\"declaration\",\"definition\",\"readonly\",\"static\","
                   "\"deprecated\",\"abstract\",\"async\",\"modification\","
                   "\"documentation\",\"defaultLibrary\"]");
    jbuf_obj_end(&cap); // end legend
    jbuf_comma(&cap);
    jbuf_key(&cap, "full");
    jbuf_bool_val(&cap, true);
    jbuf_obj_end(&cap); // end semanticTokensProvider

    jbuf_obj_end(&cap); // end capabilities

    jbuf_comma(&cap);
    jbuf_key(&cap, "serverInfo");
    jbuf_obj_start(&cap);
    jbuf_key(&cap, "name");
    jbuf_str_val(&cap, "rsg-lsp");
    jbuf_comma(&cap);
    jbuf_key(&cap, "version");
    jbuf_str_val(&cap, "1.0.0");
    jbuf_obj_end(&cap);

    jbuf_obj_end(&cap);

    send_response(srv, id, jbuf_str(&cap));
    jbuf_destroy(&cap);
}

static void handle_shutdown(LspServer *srv, int64_t id) {
    srv->shutdown_requested = true;
    send_response(srv, id, "null");
}

// ── Feature helpers ─────────────────────────────────────────────────────

/** Parse textDocument URI + position from JSON params. */
static bool parse_text_document_position(JsonValue *params, const char **out_uri, int32_t *out_line,
                                         int32_t *out_col) {
    JsonValue *td = json_get(params, "textDocument");
    JsonValue *pos = json_get(params, "position");
    if (td == NULL || pos == NULL) {
        return false;
    }
    *out_uri = json_str(json_get(td, "uri"));
    *out_line = (int32_t)json_int(json_get(pos, "line"));
    *out_col = (int32_t)json_int(json_get(pos, "character"));
    return *out_uri != NULL;
}

/** Convert a SrcLoc + URI into an LSP Location JSON string (heap-allocated). */
static char *location_to_json(const char *uri, SrcLoc loc) {
    int32_t line = loc.line > 0 ? loc.line - 1 : 0;
    int32_t col = loc.column > 0 ? loc.column - 1 : 0;

    JsonBuf b;
    jbuf_init(&b);
    jbuf_obj_start(&b);
    jbuf_key(&b, "uri");
    jbuf_str_val(&b, uri);
    jbuf_comma(&b);
    jbuf_key(&b, "range");
    jbuf_obj_start(&b);
    jbuf_key(&b, "start");
    jbuf_obj_start(&b);
    jbuf_key(&b, "line");
    jbuf_int_val(&b, line);
    jbuf_comma(&b);
    jbuf_key(&b, "character");
    jbuf_int_val(&b, col);
    jbuf_obj_end(&b);
    jbuf_comma(&b);
    jbuf_key(&b, "end");
    jbuf_obj_start(&b);
    jbuf_key(&b, "line");
    jbuf_int_val(&b, line);
    jbuf_comma(&b);
    jbuf_key(&b, "character");
    jbuf_int_val(&b, col);
    jbuf_obj_end(&b);
    jbuf_obj_end(&b);
    jbuf_obj_end(&b);

    char *result = lsp_strdup(jbuf_str(&b));
    jbuf_destroy(&b);
    return result;
}

// ── textDocument/definition ────────────────────────────────────────────

static void handle_definition(LspServer *srv, int64_t id, JsonValue *params) {
    const char *uri = NULL;
    int32_t line = 0, col = 0;
    if (!parse_text_document_position(params, &uri, &line, &col)) {
        send_response(srv, id, "null");
        return;
    }

    LspDocument *doc = find_document(srv, uri);
    if (doc == NULL || doc->text == NULL) {
        send_response(srv, id, "null");
        return;
    }

    char *word = lsp_extract_word_at(doc->text, line, col);
    if (word == NULL) {
        send_response(srv, id, "null");
        return;
    }

    const LspSymEntry *sym = lsp_find_symbol(doc->symbols, word);
    if (sym == NULL) {
        send_response(srv, id, "null");
        free(word);
        return;
    }

    // The symbol is defined in the same file — use the document URI.
    char *loc_json = location_to_json(uri, sym->loc);
    send_response(srv, id, loc_json);
    free(loc_json);
    free(word);
}

// ── textDocument/hover ─────────────────────────────────────────────────

static void handle_hover(LspServer *srv, int64_t id, JsonValue *params) {
    const char *uri = NULL;
    int32_t line = 0, col = 0;
    if (!parse_text_document_position(params, &uri, &line, &col)) {
        send_response(srv, id, "null");
        return;
    }

    LspDocument *doc = find_document(srv, uri);
    if (doc == NULL || doc->text == NULL) {
        send_response(srv, id, "null");
        return;
    }

    char *word = lsp_extract_word_at(doc->text, line, col);
    if (word == NULL) {
        send_response(srv, id, "null");
        return;
    }

    // Check builtin primitive types first.
    const char *builtin_desc = lsp_builtin_type_hover(word);
    if (builtin_desc != NULL) {
        size_t len = strlen(word) + strlen(builtin_desc) + 32;
        char *hover_text = malloc(len);
        if (hover_text != NULL) {
            snprintf(hover_text, len, "```resurg\n%s\n```\n\n%s", word, builtin_desc);
        }

        JsonBuf b;
        jbuf_init(&b);
        jbuf_obj_start(&b);
        jbuf_key(&b, "contents");
        jbuf_obj_start(&b);
        jbuf_key(&b, "kind");
        jbuf_str_val(&b, "markdown");
        jbuf_comma(&b);
        jbuf_key(&b, "value");
        jbuf_str_val(&b, hover_text);
        jbuf_obj_end(&b);
        jbuf_obj_end(&b);

        send_response(srv, id, jbuf_str(&b));
        jbuf_destroy(&b);
        free(hover_text);
        free(word);
        return;
    }

    const LspSymEntry *sym = lsp_find_symbol(doc->symbols, word);
    if (sym == NULL || sym->hover == NULL) {
        send_response(srv, id, "null");
        free(word);
        return;
    }

    JsonBuf b;
    jbuf_init(&b);
    jbuf_obj_start(&b);
    jbuf_key(&b, "contents");
    jbuf_obj_start(&b);
    jbuf_key(&b, "kind");
    jbuf_str_val(&b, "markdown");
    jbuf_comma(&b);
    jbuf_key(&b, "value");
    jbuf_str_val(&b, sym->hover);
    jbuf_obj_end(&b);
    jbuf_obj_end(&b);

    send_response(srv, id, jbuf_str(&b));
    jbuf_destroy(&b);
    free(word);
}

// ── textDocument/references ────────────────────────────────────────────

static void handle_references(LspServer *srv, int64_t id, JsonValue *params) {
    const char *uri = NULL;
    int32_t line = 0, col = 0;
    if (!parse_text_document_position(params, &uri, &line, &col)) {
        send_response(srv, id, "[]");
        return;
    }

    LspDocument *doc = find_document(srv, uri);
    if (doc == NULL || doc->text == NULL) {
        send_response(srv, id, "[]");
        return;
    }

    char *word = lsp_extract_word_at(doc->text, line, col);
    if (word == NULL) {
        send_response(srv, id, "[]");
        return;
    }

    size_t word_len = strlen(word);

    JsonBuf b;
    jbuf_init(&b);
    jbuf_arr_start(&b);

    // Scan the document text for all word-boundary occurrences.
    const char *p = doc->text;
    int32_t cur_line = 0;
    int32_t cur_col = 0;

    while (*p != '\0') {
        if (strncmp(p, word, word_len) == 0) {
            bool start_ok = (p == doc->text) || !(isalnum((unsigned char)p[-1]) || p[-1] == '_');
            bool end_ok = !isalnum((unsigned char)p[word_len]) && p[word_len] != '_';

            if (start_ok && end_ok) {
                if (b.len > 1 && b.data[b.len - 1] != '[') {
                    jbuf_comma(&b);
                }
                SrcLoc loc = {.file = NULL, .line = cur_line + 1, .column = cur_col + 1};
                char *loc_json = location_to_json(uri, loc);
                jbuf_raw(&b, loc_json);
                free(loc_json);
            }
        }

        if (*p == '\n') {
            cur_line++;
            cur_col = 0;
        } else {
            cur_col++;
        }
        p++;
    }

    jbuf_arr_end(&b);
    send_response(srv, id, jbuf_str(&b));
    jbuf_destroy(&b);
    free(word);
}

// ── textDocument/documentSymbol ────────────────────────────────────────

static void handle_document_symbol(LspServer *srv, int64_t id, JsonValue *params) {
    JsonValue *td = json_get(params, "textDocument");
    if (td == NULL) {
        send_response(srv, id, "[]");
        return;
    }
    const char *uri = json_str(json_get(td, "uri"));
    if (uri == NULL) {
        send_response(srv, id, "[]");
        return;
    }

    LspDocument *doc = find_document(srv, uri);
    if (doc == NULL) {
        send_response(srv, id, "[]");
        return;
    }

    JsonBuf b;
    jbuf_init(&b);
    jbuf_arr_start(&b);

    int32_t n = BUF_LEN(doc->symbols);
    for (int32_t i = 0; i < n; i++) {
        const LspSymEntry *s = &doc->symbols[i];
        int32_t sl = s->loc.line > 0 ? s->loc.line - 1 : 0;
        int32_t sc = s->loc.column > 0 ? s->loc.column - 1 : 0;

        if (b.len > 1 && b.data[b.len - 1] != '[') {
            jbuf_comma(&b);
        }

        jbuf_obj_start(&b);
        jbuf_key(&b, "name");
        jbuf_str_val(&b, s->name);
        jbuf_comma(&b);
        jbuf_key(&b, "kind");
        jbuf_int_val(&b, s->lsp_kind);
        jbuf_comma(&b);

        // containerName for methods/fields.
        if (s->container != NULL) {
            jbuf_key(&b, "containerName");
            jbuf_str_val(&b, s->container);
            jbuf_comma(&b);
        }

        // location
        jbuf_key(&b, "location");
        jbuf_obj_start(&b);
        jbuf_key(&b, "uri");
        jbuf_str_val(&b, uri);
        jbuf_comma(&b);
        jbuf_key(&b, "range");
        jbuf_obj_start(&b);
        jbuf_key(&b, "start");
        jbuf_obj_start(&b);
        jbuf_key(&b, "line");
        jbuf_int_val(&b, sl);
        jbuf_comma(&b);
        jbuf_key(&b, "character");
        jbuf_int_val(&b, sc);
        jbuf_obj_end(&b);
        jbuf_comma(&b);
        jbuf_key(&b, "end");
        jbuf_obj_start(&b);
        jbuf_key(&b, "line");
        jbuf_int_val(&b, sl);
        jbuf_comma(&b);
        jbuf_key(&b, "character");
        jbuf_int_val(&b, sc);
        jbuf_obj_end(&b);
        jbuf_obj_end(&b);
        jbuf_obj_end(&b);

        jbuf_obj_end(&b);
    }

    jbuf_arr_end(&b);
    send_response(srv, id, jbuf_str(&b));
    jbuf_destroy(&b);
}

// ── textDocument/semanticTokens/full ───────────────────────────────────

static void handle_semantic_tokens_full(LspServer *srv, int64_t id, JsonValue *params) {
    JsonValue *td = json_get(params, "textDocument");
    if (td == NULL) {
        send_response(srv, id, "{\"data\":[]}");
        return;
    }
    const char *uri = json_str(json_get(td, "uri"));
    if (uri == NULL) {
        send_response(srv, id, "{\"data\":[]}");
        return;
    }
    LspDocument *doc = find_document(srv, uri);
    if (doc == NULL || doc->sem_tokens == NULL) {
        send_response(srv, id, "{\"data\":[]}");
        return;
    }

    int32_t n = BUF_LEN(doc->sem_tokens);

    // Sort tokens by position for delta encoding.
    qsort(doc->sem_tokens, (size_t)n, sizeof(SemToken), lsp_sem_token_cmp);

    JsonBuf b;
    jbuf_init(&b);
    jbuf_obj_start(&b);
    jbuf_key(&b, "data");
    jbuf_arr_start(&b);

    int32_t prev_line = 0;
    int32_t prev_col = 0;
    for (int32_t i = 0; i < n; i++) {
        const SemToken *t = &doc->sem_tokens[i];
        int32_t delta_line = t->line - prev_line;
        int32_t delta_col = delta_line == 0 ? t->start_char - prev_col : t->start_char;

        if (i > 0) {
            jbuf_comma(&b);
        }
        jbuf_int_val(&b, delta_line);
        jbuf_comma(&b);
        jbuf_int_val(&b, delta_col);
        jbuf_comma(&b);
        jbuf_int_val(&b, t->length);
        jbuf_comma(&b);
        jbuf_int_val(&b, t->token_type);
        jbuf_comma(&b);
        jbuf_int_val(&b, t->modifiers);

        prev_line = t->line;
        prev_col = t->start_char;
    }

    jbuf_arr_end(&b);
    jbuf_obj_end(&b);
    send_response(srv, id, jbuf_str(&b));
    jbuf_destroy(&b);
}

// ── Document event handlers ────────────────────────────────────────────

static void handle_did_open(LspServer *srv, JsonValue *params) {
    JsonValue *td = json_get(params, "textDocument");
    if (td == NULL) {
        return;
    }
    const char *uri = json_str(json_get(td, "uri"));
    const char *text = json_str(json_get(td, "text"));
    int64_t version = json_int(json_get(td, "version"));

    if (uri == NULL || text == NULL) {
        return;
    }

    upsert_document(srv, uri, text, (int32_t)version);
    validate_document(srv, uri, text);
}

static void handle_did_change(LspServer *srv, JsonValue *params) {
    JsonValue *td = json_get(params, "textDocument");
    if (td == NULL) {
        return;
    }
    const char *uri = json_str(json_get(td, "uri"));
    int64_t version = json_int(json_get(td, "version"));

    JsonValue *changes = json_get(params, "contentChanges");
    if (uri == NULL || changes == NULL || changes->kind != JSON_ARRAY ||
        changes->array.count == 0) {
        return;
    }

    // Full sync mode: use the last content change.
    JsonValue *last_change = changes->array.items[changes->array.count - 1];
    const char *text = json_str(json_get(last_change, "text"));
    if (text == NULL) {
        return;
    }

    upsert_document(srv, uri, text, (int32_t)version);
    validate_document(srv, uri, text);
}

static void handle_did_save(LspServer *srv, JsonValue *params) {
    JsonValue *td = json_get(params, "textDocument");
    if (td == NULL) {
        return;
    }
    const char *uri = json_str(json_get(td, "uri"));

    // If includeText is set, use it. Otherwise fall back to stored doc.
    const char *text = json_str(json_get(params, "text"));
    if (text == NULL) {
        LspDocument *doc = find_document(srv, uri);
        if (doc != NULL) {
            text = doc->text;
        }
    }
    if (uri != NULL && text != NULL) {
        validate_document(srv, uri, text);
    }
}

static void handle_did_close(LspServer *srv, JsonValue *params) {
    JsonValue *td = json_get(params, "textDocument");
    if (td == NULL) {
        return;
    }
    const char *uri = json_str(json_get(td, "uri"));
    if (uri != NULL) {
        remove_document(srv, uri);
        // Clear diagnostics for closed document.
        publish_diagnostics(srv, uri, "[]");
    }
}

// ── Public API ─────────────────────────────────────────────────────────

LspServer *lsp_server_create(FILE *in, FILE *out) {
    LspServer *srv = calloc(1, sizeof(LspServer));
    if (srv == NULL) {
        return NULL;
    }
    srv->in = in;
    srv->out = out;
    return srv;
}

void lsp_server_set_std_path(LspServer *srv, const char *std_path) {
    srv->std_path = std_path;
}

int lsp_server_run(LspServer *srv) {
    for (;;) {
        char *body = lsp_transport_read(srv->in);
        if (body == NULL) {
            break; // EOF
        }

        JsonValue *msg = json_parse(body);
        free(body);
        if (msg == NULL) {
            continue;
        }

        const char *method = json_str(json_get(msg, "method"));
        JsonValue *id_val = json_get(msg, "id");
        int64_t id = id_val != NULL ? json_int(id_val) : -1;
        JsonValue *params = json_get(msg, "params");

        if (method == NULL) {
            json_free(msg);
            continue;
        }

        if (strcmp(method, "initialize") == 0) {
            handle_initialize(srv, id);
        } else if (strcmp(method, "initialized") == 0) {
            // No-op notification.
        } else if (strcmp(method, "shutdown") == 0) {
            handle_shutdown(srv, id);
        } else if (strcmp(method, "exit") == 0) {
            json_free(msg);
            return srv->shutdown_requested ? 0 : 1;
        } else if (strcmp(method, "textDocument/didOpen") == 0) {
            handle_did_open(srv, params);
        } else if (strcmp(method, "textDocument/didChange") == 0) {
            handle_did_change(srv, params);
        } else if (strcmp(method, "textDocument/didSave") == 0) {
            handle_did_save(srv, params);
        } else if (strcmp(method, "textDocument/didClose") == 0) {
            handle_did_close(srv, params);
        } else if (strcmp(method, "textDocument/definition") == 0) {
            handle_definition(srv, id, params);
        } else if (strcmp(method, "textDocument/hover") == 0) {
            handle_hover(srv, id, params);
        } else if (strcmp(method, "textDocument/references") == 0) {
            handle_references(srv, id, params);
        } else if (strcmp(method, "textDocument/documentSymbol") == 0) {
            handle_document_symbol(srv, id, params);
        } else if (strcmp(method, "textDocument/semanticTokens/full") == 0) {
            handle_semantic_tokens_full(srv, id, params);
        }
        // Unknown methods are silently ignored (per LSP spec).

        json_free(msg);
    }

    return srv->shutdown_requested ? 0 : 1;
}

void lsp_server_destroy(LspServer *srv) {
    if (srv == NULL) {
        return;
    }
    // Free all documents.
    LspDocument *doc = srv->documents;
    while (doc != NULL) {
        LspDocument *next = doc->next;
        free(doc->uri);
        free(doc->text);
        free_symbol_index(doc->symbols);
        BUF_FREE(doc->sem_tokens);
        free(doc);
        doc = next;
    }
    free(srv);
}
