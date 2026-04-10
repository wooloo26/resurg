#include "helpers.h"
#include "rsg/pass/cgen/cgen.h"

/**
 * @file c_target.c
 * @brief C-backend entry point implementing the CGenTarget contract.
 *
 * Owns the CGen lifecycle (create / destroy) and dispatches
 * cgen_emit() through the vtable to emit_file.c.
 */

// ── CGenTarget vtable callbacks ────────────────────────────────────────

static void c_target_emit(CGenTarget *self, const HirNode *file) {
    CGen *cgen = (CGen *)self;
    emit_file(cgen, file);
}

static void c_target_destroy(CGenTarget *self) {
    CGen *cgen = (CGen *)self;
    clear_compound_types(cgen);
    hash_table_destroy(&cgen->wrapper_set);
    free(cgen);
}

static bool c_target_supports_defer(const CGenTarget *self) {
    (void)self;
    return true; // C backend: goto-based cleanup
}

static bool c_target_supports_gc(const CGenTarget *self) {
    (void)self;
    return true;
}

static const char *c_target_file_extension(const CGenTarget *self) {
    (void)self;
    return ".c";
}

// ── Public API (cgen.h) ────────────────────────────────────────────────

CGenTarget *cgen_create(FILE *output, Arena *arena) {
    CGen *cgen = rsg_malloc(sizeof(*cgen));
    cgen->base.name = "c17";
    cgen->base.emit = c_target_emit;
    cgen->base.destroy = c_target_destroy;
    cgen->base.supports_defer = c_target_supports_defer;
    cgen->base.supports_gc = c_target_supports_gc;
    cgen->base.file_extension = c_target_file_extension;
    cgen->abi = runtime_abi_default();
    cgen->output = output;
    cgen->arena = arena;
    cgen->indent = 0;
    cgen->module = NULL;
    cgen->temp_counter = 0;
    cgen->str_builder_counter = 0;
    cgen->compound_types = NULL;
    cgen->defer_bodies = NULL;
    cgen->in_deferred_fn = false;
    cgen->defer_counter = 0;
    cgen->real_output = NULL;
    hash_table_init(&cgen->wrapper_set, NULL);
    return &cgen->base;
}

void cgen_destroy(CGenTarget *target) {
    if (target != NULL) {
        target->destroy(target);
    }
}

void cgen_emit(CGenTarget *target, const HirNode *file) {
    target->emit(target, file);
}
