#ifndef RSG__CHECK_H
#define RSG__CHECK_H

#include "_sema.h"
#include "rsg/pass/check/check.h"

/**
 * @file _check.h
 * @brief Internal decls shared across check translation units.
 *
 * Not part of the pub API -- only included by pass/check/ files.
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
const Type *check_call(Sema *sema, ASTNode *node);
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
const Type *check_match(Sema *sema, ASTNode *node);
const Type *check_enum_init(Sema *sema, ASTNode *node);

// ── Statement checking (check_stmt.c) ─────────────────────────────

const Type *check_if(Sema *sema, ASTNode *node);
const Type *check_block(Sema *sema, ASTNode *node);
const Type *check_var_decl(Sema *sema, ASTNode *node);
void check_fn_body(Sema *sema, ASTNode *fn_node);
const Type *check_assign(Sema *sema, ASTNode *node);
const Type *check_compound_assign(Sema *sema, ASTNode *node);

#endif // RSG__CHECK_H
