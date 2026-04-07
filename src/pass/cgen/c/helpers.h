#ifndef RSG_CGEN_C_HELPERS_H
#define RSG_CGEN_C_HELPERS_H

#include "pass/cgen/target.h"
#include "repr/types.h"

/**
 * @file helpers.h
 * @brief C-backend internal decls shared across emit translation units.
 *
 * Not part of the pub API — only included by src/pass/cgen/c/ files.
 */

// ── Struct defs ─────────────────────────────────────────────────

typedef struct CGen CGen;

/** C-backend code generator — embeds CGenTarget for vtable dispatch. */
struct CGen {
    CGenTarget base; // must be first (upcasting)
    FILE *output;
    FILE *real_output; // original output for companion fns
    Arena *arena;      // for temp str building
    int32_t indent;
    const char *module;           // current module name (may be NULL)
    int32_t temp_counter;         // monotonic counter for _rsg_tmp_N
    int32_t str_builder_counter;  // monotonic counter for _rsg_sb_N
    const Type **compound_types;  /* buf */
    const HirNode **defer_bodies; /* buf – active defers in current fn */
    bool in_deferred_fn;          // true when current fn has defers
    HashTable wrapper_set;        // dedup for fn ref wrappers
};

// ── Output helpers (emit_helpers.c) ─────────────────────────────────

/** Emit whitespace for the current indentation level. */
void emit_indent(CGen *cgen);
/** Emit fmtted text (no indent, no newline). */
void emit(CGen *cgen, const char *fmt, ...);
/** Emit an indented, newline-terminated fmtted line. */
void emit_line(CGen *cgen, const char *fmt, ...);

/** Return a fresh `_rsg_tmp_N` name. */
const char *next_temp(CGen *cgen);
/** Return a fresh `_rsg_sb_N` name. */
const char *next_str_builder(CGen *cgen);

// ── C fmtting helpers (emit_helpers.c) ───────────────────────────

/** Escape @p src for embedding inside a C str lit. */
const char *c_str_escape(const CGen *cgen, const char *src);
/** Format @p value as a C double lit with a trailing .0 when needed. */
const char *fmt_float64(const CGen *cgen, double value);
/** Format @p value as a C float lit with the f suffix. */
const char *fmt_float32(const CGen *cgen, double value);
/** Return the C char lit representation of @p c (e.g. '\\n'). */
const char *c_char_escape(const CGen *cgen, uint32_t c);

// ── Type naming (emit_helpers.c) ────────────────────────────────────

/** Return the C type name for any type, generating struct names for compounds. */
const char *c_type_for(CGen *gen, const Type *type);

// ── Compound types (emit_type.c) ────────────────────────────────────

/** Emit typedef structs for all collected compound types. */
void emit_compound_typedefs(CGen *cgen);
/** Free and reset the compound-type list. */
void clear_compound_types(CGen *cgen);

// ── Expression emission (emit_expr.c) ──────────────────────────

/** Expression dispatch — returns a C expr str for @p node. */
const char *emit_expr(CGen *cgen, const HirNode *node);

// ── Statement emission (emit_stmt.c) ───────────────────────────

/** Statement dispatch — emits a C stmt for @p node. */
void emit_stmt(CGen *cgen, const HirNode *node);
/**
 * Emit an if-expr or if-stmt.  When @p target is non-NULL the
 * result of each branch is assigned to it; otherwise emitted as a pure
 * stmt.
 */
void emit_if(CGen *cgen, const HirNode *node, const char *target, bool is_else_if);
/**
 * Emit the stmts of a block (excludes the trailing result expr
 * — the caller is responsible for that).
 */
void emit_block_stmts(CGen *cgen, const HirNode *block);

// ── File emission (emit_file.c) ────────────────────────────────

/**
 * Emit the full C translation unit: preamble, module comment, compound
 * types, forward decls, fn defs, top-level wrapper, and entry point.
 */
void emit_file(CGen *cgen, const HirNode *file);

#endif // RSG_CGEN_C_HELPERS_H
