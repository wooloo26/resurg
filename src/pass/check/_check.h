#ifndef RSG__CHECK_H
#define RSG__CHECK_H

#include "pass/resolve/_sema.h"
#include "rsg/pass/check/check.h"

/**
 * @file _check.h
 * @brief Internal decls shared across check translation units.
 *
 * Not part of the pub API -- only included by src/pass/check/ files.
 * For the semantic context (Sema, registration structs), see _sema.h.
 */

// ── Node dispatch (check_stmt.c) ──────────────────────────────────

/** Recursive AST walk - type-checks each node and returns its resolved type. */
const Type *check_node(Sema *sema, ASTNode *node);

// ── Expression checking (check_expr.c) ────────────────────────────

const Type *check_lit(Sema *sema, ASTNode *node);
const Type *check_id(Sema *sema, ASTNode *node);
const Type *check_unary(Sema *sema, ASTNode *node);
const Type *check_binary(Sema *sema, ASTNode *node);
const Type *check_member(Sema *sema, ASTNode *node);
const Type *check_idx(Sema *sema, ASTNode *node);
const Type *check_type_conversion(Sema *sema, ASTNode *node);
const Type *check_str_interpolation(Sema *sema, ASTNode *node);
const Type *check_array_lit(Sema *sema, ASTNode *node);
const Type *check_slice_lit(Sema *sema, ASTNode *node);
const Type *check_slice_expr(Sema *sema, ASTNode *node);
const Type *check_tuple_lit(Sema *sema, ASTNode *node);
const Type *check_struct_lit(Sema *sema, ASTNode *node);
const Type *check_address_of(Sema *sema, ASTNode *node);
const Type *check_deref(Sema *sema, ASTNode *node);
const Type *check_closure(Sema *sema, ASTNode *node);
void check_field_match(Sema *sema, ASTNode *value_node, const Type *expected_type);

// ── Call checking (check_call.c) ──────────────────────────────────

const Type *check_call(Sema *sema, ASTNode *node);
const Type *resolve_call(Sema *sema, ASTNode *node, const FnSig *sig);
bool is_lvalue(const ASTNode *node);
const Type *check_fn_type_call(Sema *sema, ASTNode *node, const Type *fn_type);
const Type *check_enum_variant_call(Sema *sema, ASTNode *node, const Type *enum_type,
                                    const char *variant_name);

// ── Method dispatch (check_method.c) ──────────────────────────────

const Type *check_member_call(Sema *sema, ASTNode *node, const char **out_fn_name);
const Type *check_inline_closure_call(Sema *sema, ASTNode *node);

// ── Generic call (check_generic_call.c) ───────────────────────────

const Type *check_generic_fn_call(Sema *sema, ASTNode *node, const char *fn_name);
const Type *infer_generic_call(Sema *sema, ASTNode *node, const char *fn_name);

// ── Pattern / match checking (check_match.c) ──────────────────────

const Type *check_match(Sema *sema, ASTNode *node);
const Type *check_enum_init(Sema *sema, ASTNode *node);

/** Grouped output params for match pattern coverage tracking. */
typedef struct {
    bool *variant_covered;
    bool *has_wildcard;
} MatchCoverage;

/** Check a pattern against an operand type, binding sub-pattern vars. */
void check_pattern(Sema *sema, ASTPattern *pattern, const Type *operand_type,
                   MatchCoverage *coverage);

// ── Statement checking (check_stmt.c) ─────────────────────────────

/** Type-check a fn body (used internally and by the mono pass). */
void check_fn_body(Sema *sema, ASTNode *fn_node);

const Type *check_if(Sema *sema, ASTNode *node);
const Type *check_block(Sema *sema, ASTNode *node);
const Type *check_var_decl(Sema *sema, ASTNode *node);
const Type *check_assign(Sema *sema, ASTNode *node);
const Type *check_compound_assign(Sema *sema, ASTNode *node);

// ── Decl checking (check_decl.c) ─────────────────────────────────

void check_struct_method_body(Sema *sema, ASTNode *method, const char *struct_name,
                              const Type *struct_type);
const Type *check_enum_decl_body(Sema *sema, ASTNode *node);
const Type *check_pact_decl(Sema *sema, ASTNode *node);
const Type *check_struct_decl(Sema *sema, ASTNode *node);
const Type *check_ext_decl(Sema *sema, ASTNode *node);
const Type *check_struct_destructure(Sema *sema, ASTNode *node);
const Type *check_tuple_destructure(Sema *sema, ASTNode *node);

// ── Shared helpers ─────────────────────────────────────────────────

/** Build a NODE_CALL AST node that calls EnumType.None() on @p enum_type. */
ASTNode *build_none_variant_call(Arena *arena, const Type *enum_type, SrcLoc loc);

/** Find a promoted field from embedded structs. Returns NULL if not found. */
const Type *find_promoted_field(Sema *sema, const StructDef *sdef, const char *field_name);

#endif // RSG__CHECK_H
