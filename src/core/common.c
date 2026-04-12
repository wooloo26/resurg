#include "common.h"

// ── Checked alloc wrappers ──────────────────────────────────────────

void *rsg_malloc(size_t size) {
    void *ptr = malloc(size);
    if (ptr == NULL) {
        rsg_fatal("out of memory");
    }
    return ptr;
}

void *rsg_calloc(size_t count, size_t size) {
    void *ptr = calloc(count, size);
    if (ptr == NULL) {
        rsg_fatal("out of memory");
    }
    return ptr;
}

void *rsg_realloc(void *ptr, size_t size) {
    void *result = realloc(ptr, size);
    if (result == NULL) {
        rsg_fatal("out of memory");
    }
    return result;
}

// ── Diagnostics ─────────────────────────────────────────────────────

/** Emit "label: msg\n" to @p stream with a loc prefix. */
static void emit_located_diagnostic(SrcLoc loc, const char *label, const char *fmt, va_list args) {
    fprintf(stderr, "%s:%d:%d: %s: ", loc.file, loc.line, loc.column, label);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
}

void rsg_err(SrcLoc loc, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    emit_located_diagnostic(loc, "err", fmt, args);
    va_end(args);
}

void rsg_warn(SrcLoc loc, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    emit_located_diagnostic(loc, "warning", fmt, args);
    va_end(args);
}

noreturn void rsg_fatal(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fputs("fatal: ", stderr);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    exit(1);
}
