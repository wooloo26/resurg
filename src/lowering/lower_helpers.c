#include "_lowering.h"

// ── Scope manipulation ────────────────────────────────────────────────

void lowering_scope_enter(Lowering *low) {
    LoweringScope *scope = arena_alloc(low->tt_arena, sizeof(LoweringScope));
    hash_table_init(&scope->table, low->tt_arena);
    scope->parent = low->scope;
    low->scope = scope;
}

void lowering_scope_leave(Lowering *low) {
    low->scope = low->scope->parent;
}

void lowering_scope_add(Lowering *low, const char *name, TTSym *sym) {
    // Detect shadowing: if the name already exists in any scope, mangle it.
    if (sym->mangled_name == NULL) {
        TTSym *existing = lowering_scope_find(low, name);
        if (existing != NULL) {
            sym->mangled_name = arena_sprintf(low->tt_arena, "%s__%d", name, low->shadow_counter++);
        } else {
            sym->mangled_name = name;
        }
    }
    hash_table_insert(&low->scope->table, name, sym);
}

TTSym *lowering_scope_find(const Lowering *low, const char *name) {
    for (LoweringScope *scope = low->scope; scope != NULL; scope = scope->parent) {
        TTSym *sym = hash_table_lookup(&scope->table, name);
        if (sym != NULL) {
            return sym;
        }
    }
    return NULL;
}

// ── Shared helpers ────────────────────────────────────────────────────

TTSym *lowering_make_sym(Lowering *low, const TtSymSpec *spec) {
    Sym *sema_sym = arena_alloc_zero(low->tt_arena, sizeof(Sym));
    sema_sym->name = spec->name;
    sema_sym->type = spec->type;
    switch (spec->kind) {
    case TT_SYM_VAR:
        sema_sym->kind = SYM_VAR;
        break;
    case TT_SYM_PARAM:
        sema_sym->kind = SYM_PARAM;
        break;
    case TT_SYM_FN:
        sema_sym->kind = SYM_FN;
        break;
    case TT_SYM_TYPE:
        sema_sym->kind = SYM_TYPE;
        break;
    case TT_SYM_MODULE:
        sema_sym->kind = SYM_MODULE;
        break;
    }
    return tt_sym_new(low->tt_arena, spec->kind, sema_sym, spec->is_mut, spec->loc);
}

TTSym *lowering_add_var(Lowering *low, const TtSymSpec *spec) {
    TTSym *sym = lowering_make_sym(low, spec);
    lowering_scope_add(low, spec->name, sym);
    return sym;
}

const char *lowering_make_temp_name(Lowering *low) {
    return arena_sprintf(low->tt_arena, "_tt_tmp_%d", low->temp_counter++);
}

TTNode *lowering_make_var_ref(Lowering *low, TTSym *sym, SourceLoc loc) {
    TTNode *node = tt_new(low->tt_arena, TT_VAR_REF, tt_sym_type(sym), loc);
    node->var_ref.sym = sym;
    return node;
}

TTNode *lowering_make_int_lit(Lowering *low, const IntLitSpec *spec) {
    TTNode *node = tt_new(low->tt_arena, TT_INT_LIT, spec->type, spec->loc);
    node->int_lit.value = spec->value;
    node->int_lit.int_kind = spec->int_kind;
    return node;
}

TTNode *lowering_make_var_decl(Lowering *low, TTSym *sym, TTNode *init) {
    TTNode *node = tt_new(low->tt_arena, TT_VAR_DECL, &TYPE_UNIT_INST, sym->loc);
    node->var_decl.sym = sym;
    node->var_decl.name = tt_sym_name(sym);
    node->var_decl.var_type = tt_sym_type(sym);
    node->var_decl.init = init;
    node->var_decl.is_mut = sym->is_mut;
    return node;
}

TTNode *lowering_resolve_promoted_field(Lowering *low, const FieldLookup *lookup) {
    for (int32_t i = 0; i < lookup->struct_type->struct_type.embed_count; i++) {
        const Type *embed_type = lookup->struct_type->struct_type.embedded[i];
        const StructField *sf = type_struct_find_field(embed_type, lookup->field_name);
        if (sf != NULL) {
            TTNode *embed_access =
                tt_new(low->tt_arena, TT_STRUCT_FIELD_ACCESS, embed_type, lookup->loc);
            embed_access->struct_field_access.object = lookup->object;
            embed_access->struct_field_access.field = embed_type->struct_type.name;
            embed_access->struct_field_access.via_ptr = lookup->via_ptr;

            TTNode *field_node =
                tt_new(low->tt_arena, TT_STRUCT_FIELD_ACCESS, sf->type, lookup->loc);
            field_node->struct_field_access.object = embed_access;
            field_node->struct_field_access.field = lookup->field_name;
            field_node->struct_field_access.via_ptr = false;
            return field_node;
        }
    }
    return NULL;
}

TokenKind lowering_compound_to_base_op(TokenKind op) {
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

TTNode *lowering_make_builtin_call(Lowering *low, const BuiltinCallSpec *spec) {
    TTSym *sym = lowering_scope_find(low, spec->name);
    if (sym == NULL) {
        TtSymSpec sym_spec = {TT_SYM_FN, spec->name, spec->return_type, false, spec->loc};
        sym = lowering_make_sym(low, &sym_spec);
        lowering_scope_add(low, spec->name, sym);
    }
    TTNode *callee = lowering_make_var_ref(low, sym, spec->loc);
    TTNode *node = tt_new(low->tt_arena, TT_CALL, spec->return_type, spec->loc);
    node->call.callee = callee;
    node->call.args = spec->args;
    return node;
}
