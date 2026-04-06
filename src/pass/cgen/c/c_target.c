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
    free(cgen);
}

// ── Public API (cgen.h) ────────────────────────────────────────────────

CGenTarget *cgen_create(FILE *output, Arena *arena) {
    CGen *cgen = rsg_malloc(sizeof(*cgen));
    cgen->base.emit = c_target_emit;
    cgen->base.destroy = c_target_destroy;
    cgen->output = output;
    cgen->arena = arena;
    cgen->indent = 0;
    cgen->module = NULL;
    cgen->temp_counter = 0;
    cgen->str_builder_counter = 0;
    cgen->compound_types = NULL;
    cgen->defer_bodies = NULL;
    cgen->in_deferred_fn = false;
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
