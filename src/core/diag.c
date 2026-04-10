#include "core/diag.h"

/**
 * @file diag.c
 * @brief Diagnostic emission and storage.
 *
 * Rendering (text, JSON, source snippets) lives in diag_render.c.
 */

// ── DiagCtx lifecycle ──────────────────────────────────────────────────

void diag_ctx_init(DiagCtx *dctx, Arena *arena) {
    dctx->arena = arena;
    dctx->diags = NULL;
    dctx->err_count = 0;
    dctx->warn_count = 0;
}

void diag_ctx_destroy(DiagCtx *dctx) {
    // Free the related bufs inside each diagnostic.
    for (int32_t i = 0; i < BUF_LEN(dctx->diags); i++) {
        BUF_FREE(dctx->diags[i].related);
    }
    BUF_FREE(dctx->diags);
    dctx->err_count = 0;
    dctx->warn_count = 0;
}

// ── Internal: format a message into the arena ──────────────────────────

static const char *diag__vformat(Arena *arena, const char *fmt, va_list args) {
    va_list copy;
    va_copy(copy, args);
    int32_t len = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);

    char *buf = arena_alloc(arena, (size_t)len + 1);
    vsnprintf(buf, (size_t)len + 1, fmt, args);
    return buf;
}

/** Push a new diagnostic and update counters. */
static Diagnostic *diag__push(DiagCtx *dctx, DiagLevel level, SrcLoc loc, SrcLoc end_loc,
                              const char *code, const char *message) {
    Diagnostic diag = {
        .level = level,
        .loc = loc,
        .end_loc = end_loc,
        .code = code,
        .message = message,
        .related = NULL,
    };
    BUF_PUSH(dctx->diags, diag);
    if (level == DIAG_ERR) {
        dctx->err_count++;
    } else if (level == DIAG_WARN) {
        dctx->warn_count++;
    }
    return &dctx->diags[BUF_LEN(dctx->diags) - 1];
}

/** Attach a related diagnostic to the last parent. */
static void diag__attach(DiagCtx *dctx, DiagLevel level, SrcLoc loc, const char *message) {
    if (BUF_LEN(dctx->diags) == 0) {
        return;
    }
    Diagnostic *parent = &dctx->diags[BUF_LEN(dctx->diags) - 1];
    Diagnostic related = {
        .level = level,
        .loc = loc,
        .end_loc = loc,
        .code = NULL,
        .message = message,
        .related = NULL,
    };
    BUF_PUSH(parent->related, related);
}

// ── Public API ─────────────────────────────────────────────────────────

void diag_emit(DiagCtx *dctx, DiagLevel level, SrcLoc loc, SrcLoc end_loc, const char *code,
               const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const char *msg = diag__vformat(dctx->arena, fmt, args);
    va_end(args);
    diag__push(dctx, level, loc, end_loc, code, msg);
}

void diag_at(DiagCtx *dctx, DiagLevel level, SrcLoc loc, const char *code, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const char *msg = diag__vformat(dctx->arena, fmt, args);
    va_end(args);
    diag__push(dctx, level, loc, loc, code, msg);
}

void diag_attach_note(DiagCtx *dctx, SrcLoc loc, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const char *msg = diag__vformat(dctx->arena, fmt, args);
    va_end(args);
    diag__attach(dctx, DIAG_NOTE, loc, msg);
}

void diag_attach_help(DiagCtx *dctx, SrcLoc loc, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const char *msg = diag__vformat(dctx->arena, fmt, args);
    va_end(args);
    diag__attach(dctx, DIAG_HELP, loc, msg);
}

bool diag_has_errors(const DiagCtx *dctx) {
    return dctx->err_count > 0;
}

int32_t diag_count(const DiagCtx *dctx) {
    return BUF_LEN(dctx->diags);
}
