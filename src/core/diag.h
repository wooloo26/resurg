#ifndef RSG_DIAG_H
#define RSG_DIAG_H

#include "core/common.h"

/**
 * @file diag.h
 * @brief Structured diagnostic system — collectable, renderable diagnostics.
 *
 * Replaces direct `fprintf(stderr, ...)` with a structured representation that
 * can be collected, filtered, and consumed by tools (LSP, linter, formatter).
 *
 * Typical usage:
 * @code
 *     DiagCtx dctx;
 *     diag_ctx_init(&dctx, arena);
 *     diag_emit(&dctx, DIAG_ERR, loc, end_loc, "E0001", "undefined variable '%s'", name);
 *     diag_attach_note(&dctx, loc, "declared here");
 *     diag_render_all(&dctx, stderr);
 *     diag_ctx_destroy(&dctx);
 * @endcode
 */

// ── Severity ───────────────────────────────────────────────────────────

/** Diagnostic severity level. */
typedef enum {
    DIAG_ERR,  // Compilation err — prevents codegen.
    DIAG_WARN, // Warning — does not prevent codegen.
    DIAG_NOTE, // Attached note — provides context for a parent diagnostic.
    DIAG_HELP, // Attached help — suggests a fix.
} DiagLevel;

// ── Diagnostic ─────────────────────────────────────────────────────────

typedef struct Diagnostic Diagnostic;

/**
 * A single diagnostic message, optionally spanning a source range
 * and carrying an err code.  Notes/help diagnostics are attached to
 * the parent via a stretchy buf.
 */
struct Diagnostic {
    DiagLevel level;
    SrcLoc loc;          // start of the span (file:line:col)
    SrcLoc end_loc;      // end of the span (for underline); zero-initialised means point diagnostic
    const char *code;    // err code (e.g. "E0001"), or NULL
    const char *message; // human-readable message (arena-owned)
    Diagnostic *related; /* buf — attached notes/help */
};

// ── DiagCtx ────────────────────────────────────────────────────────────

/**
 * Diagnostic collector — accumulates diagnostics for a compilation unit.
 *
 * Diagnostics are arena-allocated.  The collector maintains separate
 * counters for errs and warnings.
 */
typedef struct DiagCtx {
    Arena *arena;       // arena for diagnostic message strings
    Diagnostic *diags;  /* buf — all emitted diagnostics (heap) */
    int32_t err_count;  // total errs emitted
    int32_t warn_count; // total warnings emitted
} DiagCtx;

/** Initialise a diagnostic context. @p arena is used for message strings. */
void diag_ctx_init(DiagCtx *dctx, Arena *arena);

/** Free heap-owned diagnostic bufs (arena memory is not freed). */
void diag_ctx_destroy(DiagCtx *dctx);

/**
 * Emit a diagnostic at the given location.
 *
 * @param dctx   Diagnostic context.
 * @param level  Severity level.
 * @param loc    Start location.
 * @param end_loc End location (pass @c loc for a point diagnostic).
 * @param code   Err code string (e.g. "E0001"), or NULL.
 * @param fmt    printf-style format string.
 */
void diag_emit(DiagCtx *dctx, DiagLevel level, SrcLoc loc, SrcLoc end_loc, const char *code,
               const char *fmt, ...);

/**
 * Emit a diagnostic with only a start location (point diagnostic).
 * Convenience wrapper around diag_emit().
 */
void diag_at(DiagCtx *dctx, DiagLevel level, SrcLoc loc, const char *code, const char *fmt, ...);

/**
 * Attach a note to the most recently emitted diagnostic.
 * No-op if no diagnostics have been emitted yet.
 */
void diag_attach_note(DiagCtx *dctx, SrcLoc loc, const char *fmt, ...);

/**
 * Attach a help message to the most recently emitted diagnostic.
 * No-op if no diagnostics have been emitted yet.
 */
void diag_attach_help(DiagCtx *dctx, SrcLoc loc, const char *fmt, ...);

/** Return true if any errs have been emitted. */
bool diag_has_errors(const DiagCtx *dctx);

/** Return the number of diagnostics in the context. */
int32_t diag_count(const DiagCtx *dctx);

// ── Rendering ──────────────────────────────────────────────────────────

/**
 * Render all collected diagnostics to @p out.
 *
 * Format:
 * @code
 *     file:line:col: err[E0001]: message
 *       --> file:line:col
 *       |
 *    42 | let x: i32 = "hello";
 *       |              ^^^^^^^ type mismatch
 *       |
 *       = note: expected 'i32', got 'str'
 *       = help: use a type conversion
 * @endcode
 *
 * Falls back to simple one-line format when source is unavailable.
 */
void diag_render_all(const DiagCtx *dctx, FILE *out);

/**
 * Render a single diagnostic (without source snippets) in the classic
 * "file:line:col: level: message" format.  Used as a lightweight
 * fallback when source text is not available.
 */
void diag_render_simple(const Diagnostic *diag, FILE *out);

/**
 * Render all collected diagnostics as a JSON array to @p out.
 *
 * Each diagnostic is a JSON object with fields:
 * @c severity, @c file, @c line, @c column, @c end_line, @c end_column,
 * @c code (nullable), @c message, and @c related (array of child diagnostics).
 *
 * Output is compatible with LSP @c PublishDiagnostics and machine-readable
 * tools (linters, editors, CI).
 */
void diag_render_json(const DiagCtx *dctx, FILE *out);

/** Return the human-readable label for a DiagLevel. */
const char *diag_level_str(DiagLevel level);

#endif // RSG_DIAG_H
