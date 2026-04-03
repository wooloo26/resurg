#ifndef RG__CODEGEN_H
#define RG__CODEGEN_H

#include "codegen.h"
#include "types.h"

/**
 * @file _codegen.h
 * @brief Internal declarations shared across codegen translation units.
 *
 * Not part of the public API -- only included by src/codegen/ files.
 */

// ── Struct definitions ─────────────────────────────────────────────────

/** Maps a Resurg variable name to its (possibly mangled) C identifier. */
struct VariableEntry {
    const char *rsg_name;
    const char *c_name; // may differ when shadowing is resolved
};

struct CodeGenerator {
    FILE *output;
    Arena *arena; // for temporary string building
    int32_t indent;
    const char *module;              // current module name (may be NULL)
    const char *source_file;         // escaped source path for rsg_assert
    int32_t temporary_counter;       // monotonic counter for _rsg_tmp_N
    int32_t string_builder_counter;  // monotonic counter for _rsg_sb_N
    VariableEntry *variables;        /* buf */
    int32_t shadow_variable_counter; // suffix counter for shadowed renames
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

// ── Variable tracking (codegen_helpers.c) ──────────────────────────────

/** Look up the C name for a Resurg variable. */
const char *codegen_variable_lookup(const CodeGenerator *generator, const char *name);
/** Register a new variable, returning the (possibly suffixed) C name. */
const char *codegen_variable_define(CodeGenerator *generator, const char *name);
/** Clear the variable table (call at each function boundary). */
void codegen_variable_scope_reset(CodeGenerator *generator);

/**
 * Prefix function names with `rsg_` to avoid C reserved-word and
 * stdlib collisions.
 */
const char *codegen_mangle_function_name(CodeGenerator *generator, const char *name);

// ── C formatting helpers (codegen_helpers.c) ───────────────────────────

/** Map Resurg binary operator TokenKind to its C operator string. */
const char *codegen_c_binary_operator(TokenKind op);
/** Map Resurg compound-assignment operator to its C string. */
const char *codegen_c_compound_operator(TokenKind op);
/** Escape @p source for embedding inside a C string literal. */
const char *codegen_c_string_escape(const CodeGenerator *generator, const char *source);
/** Escape a file path for embedding in C (backslash -> forward slash). */
const char *codegen_c_escape_file_path(const CodeGenerator *generator, const char *path);
/** Format @p value as a C double literal with a trailing .0 when needed. */
const char *codegen_format_float64(const CodeGenerator *generator, double value);
/** Format @p value as a C float literal with the f suffix. */
const char *codegen_format_float32(const CodeGenerator *generator, double value);
/** Return the C char literal representation of @p c (e.g. '\\n'). */
const char *codegen_c_char_escape(const CodeGenerator *generator, char c);

// ── Type naming (codegen_helpers.c) ────────────────────────────────────

/** Return the C type name for any type, generating struct names for compounds. */
const char *codegen_c_type_for(CodeGenerator *gen, const Type *type);

// ── Compound types (codegen_types.c) ───────────────────────────────────

/** Recursively walk @p node collecting all array/tuple types. */
void codegen_collect_compound_types(const ASTNode *node);
/** Emit typedef structs for all collected compound types. */
void codegen_emit_compound_typedefs(CodeGenerator *generator);
/** Free and reset the global compound-type list. */
void codegen_reset_compound_types(void);

// ── Expression emission (codegen_expression.c) ─────────────────────────

/** Expression dispatch — returns a C expression string for @p node. */
const char *codegen_emit_expression(CodeGenerator *generator, const ASTNode *node);

// ── Statement emission (codegen_statement.c) ───────────────────────────

/** Statement dispatch — emits a C statement for @p node. */
void codegen_emit_statement(CodeGenerator *generator, const ASTNode *node);
/**
 * Emit an if-expression or if-statement.  When @p target is non-NULL the
 * result of each branch is assigned to it; otherwise emitted as a pure
 * statement.
 */
void codegen_emit_if(CodeGenerator *generator, const ASTNode *node, const char *target, bool is_else_if);
/**
 * Emit the statements of a block (excludes the trailing result expression
 * — the caller is responsible for that).
 */
void codegen_emit_block_statements(CodeGenerator *generator, const ASTNode *block);

#endif // RG__CODEGEN_H
