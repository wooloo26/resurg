#ifndef RG__CODEGEN_H
#define RG__CODEGEN_H

#include "rsg/cgen.h"
#include "types/types.h"

/**
 * @file _cgen.h
 * @brief Internal decls shared across codegen translation units.
 *
 * Not part of the pub API -- only included by lib/backend/c/ files.
 */

// ── Struct defs ─────────────────────────────────────────────────

struct CGen {
    FILE *output;
    Arena *arena; // for temp str building
    int32_t indent;
    const char *module;          // current module name (may be NULL)
    int32_t temp_counter;        // monotonic counter for _rsg_tmp_N
    int32_t str_builder_counter; // monotonic counter for _rsg_sb_N
    const Type **compound_types; /* buf */
    const TTNode **defer_bodies; /* buf – active defers in current fn */
    bool in_deferred_fn;         // true when current fn has defers
};

// ── Output helpers (cgen_helpers.c) ─────────────────────────────────

/** Emit whitespace for the current indentation level. */
void cgen_emit_indent(CGen *cgen);
/** Emit fmtted text (no indent, no newline). */
void _cgen_emit(CGen *cgen, const char *fmt, ...);
/** Emit an indented, newline-terminated fmtted line. */
void cgen_emit_line(CGen *cgen, const char *fmt, ...);

/** Return a fresh `_rsg_tmp_N` name. */
const char *cgen_next_temp(CGen *cgen);
/** Return a fresh `_rsg_sb_N` name. */
const char *cgen_next_str_builder(CGen *cgen);

// ── C fmtting helpers (cgen_helpers.c) ───────────────────────────

/** Escape @p source for embedding inside a C str lit. */
const char *cgen_c_str_escape(const CGen *cgen, const char *source);
/** Format @p value as a C double lit with a trailing .0 when needed. */
const char *cgen_fmt_float64(const CGen *cgen, double value);
/** Format @p value as a C float lit with the f suffix. */
const char *cgen_fmt_float32(const CGen *cgen, double value);
/** Return the C char lit representation of @p c (e.g. '\\n'). */
const char *cgen_c_char_escape(const CGen *cgen, uint32_t c);

// ── Type naming (cgen_helpers.c) ────────────────────────────────────

/** Return the C type name for any type, generating struct names for compounds. */
const char *cgen_c_type_for(CGen *gen, const Type *type);

// ── Compound types (cgen_types.c) ───────────────────────────────────

/** Emit typedef structs for all collected compound types. */
void cgen_emit_compound_typedefs(CGen *cgen);
/** Free and reset the compound-type list. */
void cgen_clear_compound_types(CGen *cgen);

// ── Expression emission (cgen_expr.c) ─────────────────────────

/** Expression dispatch — returns a C expr str for @p node. */
const char *cgen_emit_expr(CGen *cgen, const TTNode *node);

// ── Statement emission (cgen_stmt.c) ───────────────────────────

/** Statement dispatch — emits a C stmt for @p node. */
void cgen_emit_stmt(CGen *cgen, const TTNode *node);
/**
 * Emit an if-expr or if-stmt.  When @p target is non-NULL the
 * result of each branch is assigned to it; otherwise emitted as a pure
 * stmt.
 */
void cgen_emit_if(CGen *cgen, const TTNode *node, const char *target, bool is_else_if);
/**
 * Emit the stmts of a block (excludes the trailing result expr
 * — the caller is responsible for that).
 */
void cgen_emit_block_stmts(CGen *cgen, const TTNode *block);

#endif // RG__CODEGEN_H
