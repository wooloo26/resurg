#ifndef RSG_CGEN_TARGET_H
#define RSG_CGEN_TARGET_H

#include "repr/hir.h"

/**
 * @file target.h
 * @brief Unified backend interface for code generation.
 *
 * Each backend (C, C++, …) embeds a CGenTarget as its first member
 * and populates the vtable.  The driver calls through the vtable
 * without knowing the concrete backend type.
 *
 * Required methods: emit, destroy.
 * Optional methods (NULL → default behaviour): supports_defer,
 * supports_gc, file_extension.
 */
typedef struct CGenTarget CGenTarget;

struct CGenTarget {
    /** Human-readable backend name (e.g. "c17", "llvm", "js"). */
    const char *name;
    /** Emit a full translation unit for @p file. */
    void (*emit)(CGenTarget *self, const HirNode *file);
    /** Destroy the target and free all owned resrcs. */
    void (*destroy)(CGenTarget *self);

    // ── Optional capability queries ────────────────────────────────
    /** Return true if the backend supports defer via goto cleanup / try-finally. */
    bool (*supports_defer)(const CGenTarget *self);
    /** Return true if the backend requires GC runtime support. */
    bool (*supports_gc)(const CGenTarget *self);
    /** Return the output file extension (e.g. ".c", ".ll", ".js"). */
    const char *(*file_extension)(const CGenTarget *self);
};

// ── Capability query helpers (NULL-safe) ───────────────────────────────

/** Query whether the target supports defer.  Returns false if unset. */
static inline bool cgen_target_supports_defer(const CGenTarget *target) {
    return target->supports_defer != NULL && target->supports_defer(target);
}

/** Query whether the target supports GC.  Returns false if unset. */
static inline bool cgen_target_supports_gc(const CGenTarget *target) {
    return target->supports_gc != NULL && target->supports_gc(target);
}

/** Query the file extension.  Returns ".c" if unset. */
static inline const char *cgen_target_file_extension(const CGenTarget *target) {
    if (target->file_extension != NULL) {
        return target->file_extension(target);
    }
    return ".c";
}

#endif // RSG_CGEN_TARGET_H
