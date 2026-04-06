#ifndef RG__CODEGEN_H
#define RG__CODEGEN_H

#include "rsg/codegen.h"
#include "types/types.h"

/**
 * @file _codegen.h
 * @brief Internal declarations shared across codegen translation units.
 *
 * Not part of the public API -- only included by lib/backend/c/ files.
 */

// ── Struct definitions ─────────────────────────────────────────────────

struct CodeGenerator {
    FILE *output;
    Arena *arena; // for temporary string building
    int32_t indent;
    const char *module;             // current module name (may be NULL)
    int32_t temporary_counter;      // monotonic counter for _rsg_tmp_N
    int32_t string_builder_counter; // monotonic counter for _rsg_sb_N
    const Type **compound_types;    /* buf */
    const TtNode **defer_bodies;    /* buf – active defers in current function */
    bool in_deferred_function;      // true when current function has defers
};

// ── Output helpers (codegen_helpers.c) ─────────────────────────────────

/** Emit whitespace for the current indentation level. */
void codegen_emit_indent(CodeGenerator *generator);
/** Emit formatted text (no indent, no newline). */
void codegen_emit(CodeGenerator *generator, const char *format, ...);
/** Emit an indented, newline-terminated formatted line. */
void codegen_emit_line(CodeGenerator *generator, const char *format, ...);

/** Return a fresh `_rsg_tmp_N` name. */
const char *codegen_next_temporary(CodeGenerator *generator);
/** Return a fresh `_rsg_sb_N` name. */
const char *codegen_next_string_builder(CodeGenerator *generator);

// ── C formatting helpers (codegen_helpers.c) ───────────────────────────

/** Escape @p source for embedding inside a C string literal. */
const char *codegen_c_string_escape(const CodeGenerator *generator, const char *source);
/** Format @p value as a C double literal with a trailing .0 when needed. */
const char *codegen_format_float64(const CodeGenerator *generator, double value);
/** Format @p value as a C float literal with the f suffix. */
const char *codegen_format_float32(const CodeGenerator *generator, double value);
/** Return the C char literal representation of @p c (e.g. '\\n'). */
const char *codegen_c_char_escape(const CodeGenerator *generator, uint32_t c);

// ── Type naming (codegen_helpers.c) ────────────────────────────────────

/** Return the C type name for any type, generating struct names for compounds. */
const char *codegen_c_type_for(CodeGenerator *gen, const Type *type);

// ── Compound types (codegen_types.c) ───────────────────────────────────

/** Emit typedef structs for all collected compound types. */
void codegen_emit_compound_typedefs(CodeGenerator *generator);
/** Free and reset the compound-type list. */
void codegen_clear_compound_types(CodeGenerator *generator);

// ── Expression emission (codegen_expression.c) ─────────────────────────

/** Expression dispatch — returns a C expression string for @p node. */
const char *codegen_emit_expression(CodeGenerator *generator, const TtNode *node);

// ── Statement emission (codegen_statement.c) ───────────────────────────

/** Statement dispatch — emits a C statement for @p node. */
void codegen_emit_statement(CodeGenerator *generator, const TtNode *node);
/**
 * Emit an if-expression or if-statement.  When @p target is non-NULL the
 * result of each branch is assigned to it; otherwise emitted as a pure
 * statement.
 */
void codegen_emit_if(CodeGenerator *generator, const TtNode *node, const char *target,
                     bool is_else_if);
/**
 * Emit the statements of a block (excludes the trailing result expression
 * — the caller is responsible for that).
 */
void codegen_emit_block_statements(CodeGenerator *generator, const TtNode *block);

#endif // RG__CODEGEN_H
