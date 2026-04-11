#include "_check.h"

/**
 * @file check_helpers.c
 * @brief Shared helpers used across multiple check translation units.
 *
 * Centralises cross-cutting utilities (lvalue detection, foreign-struct
 * checks, promoted field lookup, AST node construction) so they are not
 * scattered through expression- or call-specific files.
 */

// ── Addressability ────────────────────────────────────────────────────

bool is_lvalue(const ASTNode *node) {
    if (node == NULL) {
        return false;
    }
    switch (node->kind) {
    case NODE_ID:
    case NODE_MEMBER:
    case NODE_IDX:
    case NODE_ADDRESS_OF:
        return true;
    default:
        return false;
    }
}

// ── Foreign-struct detection ──────────────────────────────────────────

/** Return true when @p struct_name belongs to a foreign module and the
 *  current context is NOT inside an ext block for that type. */
bool is_foreign_struct(const Sema *sema, const char *struct_name) {
    const char *dot = strchr(struct_name, '.');
    if (dot == NULL) {
        return false; // same-file struct
    }
    if (sema->infer.self_type_name != NULL &&
        strcmp(sema->infer.self_type_name, struct_name) == 0) {
        return false; // inside ext block for this type
    }
    if (sema->module.current != NULL) {
        size_t mod_len = strlen(sema->module.current);
        if (strncmp(struct_name, sema->module.current, mod_len) == 0 &&
            struct_name[mod_len] == '.') {
            return false; // same-module struct
        }
    }
    return true;
}

// ── AST construction helpers ──────────────────────────────────────────

ASTNode *build_unit_variant_call(Arena *arena, const Type *enum_type, const char *variant_name,
                                 SrcLoc loc) {
    ASTNode *call = ast_new(arena, NODE_CALL, loc);
    ASTNode *callee = ast_new(arena, NODE_MEMBER, loc);
    callee->member.object = ast_new(arena, NODE_ID, loc);
    callee->member.object->id.name = type_enum_name(enum_type);
    callee->member.object->type = enum_type;
    callee->member.member = variant_name;
    call->call.callee = callee;
    call->call.args = NULL;
    call->call.arg_names = NULL;
    call->call.arg_is_mut = NULL;
    call->call.type_args = NULL;
    call->call.variadic_start = -1;
    call->type = enum_type;
    return call;
}

// ── Promoted field lookup ─────────────────────────────────────────────

const Type *find_promoted_field(Sema *sema, const StructDef *sdef, const char *field_name) {
    for (int32_t ei = 0; ei < BUF_LEN(sdef->embedded); ei++) {
        StructDef *embed_def = sema_lookup_struct(sema, sdef->embedded[ei]);
        if (embed_def != NULL) {
            for (int32_t fi = 0; fi < BUF_LEN(embed_def->fields); fi++) {
                if (strcmp(embed_def->fields[fi].name, field_name) == 0) {
                    return embed_def->fields[fi].type;
                }
            }
        }
    }
    return NULL;
}
