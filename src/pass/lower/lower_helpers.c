#include "_lower.h"

// ── Scope manipulation ────────────────────────────────────────────────

void lower_scope_enter(Lower *low) {
    LoweringScope *scope = arena_alloc(low->hir_arena, sizeof(LoweringScope));
    hash_table_init(&scope->table, low->hir_arena);
    scope->parent = low->scope;
    low->scope = scope;
}

void lower_scope_leave(Lower *low) {
    low->scope = low->scope->parent;
}

void lower_scope_define(Lower *low, const char *name, HirSym *sym) {
    // Detect shadowing: if the name already exists in any scope, mangle it.
    if (sym->mangled_name == NULL) {
        HirSym *existing = lower_scope_lookup(low, name);
        if (existing != NULL) {
            sym->mangled_name =
                arena_sprintf(low->hir_arena, "%s__%d", name, low->shadow_counter++);
        } else {
            sym->mangled_name = name;
        }
    }
    hash_table_insert(&low->scope->table, name, sym);
}

HirSym *lower_scope_lookup(const Lower *low, const char *name) {
    for (LoweringScope *scope = low->scope; scope != NULL; scope = scope->parent) {
        HirSym *sym = hash_table_lookup(&scope->table, name);
        if (sym != NULL) {
            return sym;
        }
    }
    return NULL;
}

// ── Shared helpers ────────────────────────────────────────────────────

HirSym *lower_make_sym(Lower *low, const HirSymSpec *spec) {
    return hir_sym_new(low->hir_arena, spec);
}

HirSym *lower_add_var(Lower *low, const HirSymSpec *spec) {
    HirSym *sym = lower_make_sym(low, spec);
    lower_scope_define(low, spec->name, sym);
    return sym;
}

const char *lower_make_temp_name(Lower *low) {
    return arena_sprintf(low->hir_arena, "_hir_tmp_%d", low->temp_counter++);
}

HirNode *lower_make_var_ref(Lower *low, HirSym *sym, SrcLoc loc) {
    HirNode *node = hir_new(low->hir_arena, HIR_VAR_REF, hir_sym_type(sym), loc);
    node->var_ref.sym = sym;
    return node;
}

HirNode *lower_make_int_lit(Lower *low, const IntLitSpec *spec) {
    HirNode *node = hir_new(low->hir_arena, HIR_INT_LIT, spec->type, spec->loc);
    node->int_lit.value = spec->value;
    node->int_lit.int_kind = spec->int_kind;
    return node;
}

HirNode *lower_make_var_decl(Lower *low, HirSym *sym, HirNode *init) {
    HirNode *node = hir_new(low->hir_arena, HIR_VAR_DECL, &TYPE_UNIT_INST, sym->loc);
    node->var_decl.sym = sym;
    node->var_decl.name = hir_sym_name(sym);
    node->var_decl.var_type = hir_sym_type(sym);
    node->var_decl.init = init;
    node->var_decl.is_mut = sym->is_mut;
    return node;
}

HirNode *lower_resolve_promoted_field(Lower *low, const FieldLookup *lookup) {
    for (int32_t i = 0; i < lookup->struct_type->struct_type.embed_count; i++) {
        const Type *embed_type = lookup->struct_type->struct_type.embedded[i];
        const StructField *sf = type_struct_find_field(embed_type, lookup->field_name);
        if (sf != NULL) {
            HirNode *embed_access =
                hir_new(low->hir_arena, HIR_STRUCT_FIELD_ACCESS, embed_type, lookup->loc);
            embed_access->struct_field_access.object = lookup->object;
            embed_access->struct_field_access.field = embed_type->struct_type.name;
            embed_access->struct_field_access.via_ptr = lookup->via_ptr;

            HirNode *field_node =
                hir_new(low->hir_arena, HIR_STRUCT_FIELD_ACCESS, sf->type, lookup->loc);
            field_node->struct_field_access.object = embed_access;
            field_node->struct_field_access.field = lookup->field_name;
            field_node->struct_field_access.via_ptr = false;
            return field_node;
        }
    }
    return NULL;
}

TokenKind lower_compound_to_base_op(TokenKind op) {
    switch (op) {
    case TOKEN_PLUS_EQUAL:
        return TOKEN_PLUS;
    case TOKEN_MINUS_EQUAL:
        return TOKEN_MINUS;
    case TOKEN_STAR_EQUAL:
        return TOKEN_STAR;
    case TOKEN_SLASH_EQUAL:
        return TOKEN_SLASH;
    default:
        return op;
    }
}

HirNode *lower_make_builtin_call(Lower *low, const BuiltinCallSpec *spec) {
    HirSym *sym = lower_scope_lookup(low, spec->name);
    if (sym == NULL) {
        HirSymSpec sym_spec = {HIR_SYM_FN, spec->name, spec->return_type, false, spec->loc};
        sym = lower_make_sym(low, &sym_spec);
        lower_scope_define(low, spec->name, sym);
    }
    HirNode *callee = lower_make_var_ref(low, sym, spec->loc);
    HirNode *node = hir_new(low->hir_arena, HIR_CALL, spec->return_type, spec->loc);
    node->call.callee = callee;
    node->call.args = spec->args;
    return node;
}
