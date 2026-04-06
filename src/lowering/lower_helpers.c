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

void lowering_scope_add(Lowering *low, const char *name, TtSymbol *symbol) {
    // Detect shadowing: if the name already exists in any scope, mangle it.
    if (symbol->mangled_name == NULL) {
        TtSymbol *existing = lowering_scope_find(low, name);
        if (existing != NULL) {
            symbol->mangled_name =
                arena_sprintf(low->tt_arena, "%s__%d", name, low->shadow_counter++);
        } else {
            symbol->mangled_name = name;
        }
    }
    hash_table_insert(&low->scope->table, name, symbol);
}

TtSymbol *lowering_scope_find(const Lowering *low, const char *name) {
    for (LoweringScope *scope = low->scope; scope != NULL; scope = scope->parent) {
        TtSymbol *symbol = hash_table_lookup(&scope->table, name);
        if (symbol != NULL) {
            return symbol;
        }
    }
    return NULL;
}

// ── Shared helpers ────────────────────────────────────────────────────

TtSymbol *lowering_make_symbol(Lowering *low, const TtSymbolSpec *spec) {
    Symbol *sema_sym = arena_alloc_zero(low->tt_arena, sizeof(Symbol));
    sema_sym->name = spec->name;
    sema_sym->type = spec->type;
    switch (spec->kind) {
    case TT_SYMBOL_VARIABLE:
        sema_sym->kind = SYM_VAR;
        break;
    case TT_SYMBOL_PARAMETER:
        sema_sym->kind = SYM_PARAM;
        break;
    case TT_SYMBOL_FUNCTION:
        sema_sym->kind = SYM_FUNCTION;
        break;
    case TT_SYMBOL_TYPE:
        sema_sym->kind = SYM_TYPE;
        break;
    case TT_SYMBOL_MODULE:
        sema_sym->kind = SYM_MODULE;
        break;
    }
    return tt_symbol_new(low->tt_arena, spec->kind, sema_sym, spec->is_mut, spec->location);
}

TtSymbol *lowering_add_variable(Lowering *low, const TtSymbolSpec *spec) {
    TtSymbol *sym = lowering_make_symbol(low, spec);
    lowering_scope_add(low, spec->name, sym);
    return sym;
}

const char *lowering_make_temp_name(Lowering *low) {
    return arena_sprintf(low->tt_arena, "_tt_tmp_%d", low->temp_counter++);
}

TtNode *lowering_make_var_ref(Lowering *low, TtSymbol *symbol, SourceLocation location) {
    TtNode *node = tt_new(low->tt_arena, TT_VARIABLE_REFERENCE, tt_symbol_type(symbol), location);
    node->variable_reference.symbol = symbol;
    return node;
}

TtNode *lowering_make_int_lit(Lowering *low, const IntLitSpec *spec) {
    TtNode *node = tt_new(low->tt_arena, TT_INT_LITERAL, spec->type, spec->location);
    node->int_literal.value = spec->value;
    node->int_literal.int_kind = spec->int_kind;
    return node;
}

TtNode *lowering_make_var_decl(Lowering *low, TtSymbol *symbol, TtNode *initializer) {
    TtNode *node =
        tt_new(low->tt_arena, TT_VARIABLE_DECLARATION, &TYPE_UNIT_INSTANCE, symbol->location);
    node->variable_declaration.symbol = symbol;
    node->variable_declaration.name = tt_symbol_name(symbol);
    node->variable_declaration.var_type = tt_symbol_type(symbol);
    node->variable_declaration.initializer = initializer;
    node->variable_declaration.is_mut = symbol->is_mut;
    return node;
}

TtNode *lowering_resolve_promoted_field(Lowering *low, const FieldLookup *lookup) {
    for (int32_t i = 0; i < lookup->struct_type->struct_type.embed_count; i++) {
        const Type *embed_type = lookup->struct_type->struct_type.embedded[i];
        const StructField *sf = type_struct_find_field(embed_type, lookup->field_name);
        if (sf != NULL) {
            TtNode *embed_access =
                tt_new(low->tt_arena, TT_STRUCT_FIELD_ACCESS, embed_type, lookup->location);
            embed_access->struct_field_access.object = lookup->object;
            embed_access->struct_field_access.field = embed_type->struct_type.name;
            embed_access->struct_field_access.via_pointer = lookup->via_pointer;

            TtNode *field_node =
                tt_new(low->tt_arena, TT_STRUCT_FIELD_ACCESS, sf->type, lookup->location);
            field_node->struct_field_access.object = embed_access;
            field_node->struct_field_access.field = lookup->field_name;
            field_node->struct_field_access.via_pointer = false;
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

TtNode *lowering_make_builtin_call(Lowering *low, const BuiltinCallSpec *spec) {
    TtSymbol *sym = lowering_scope_find(low, spec->name);
    if (sym == NULL) {
        TtSymbolSpec sym_spec = {TT_SYMBOL_FUNCTION, spec->name, spec->return_type, false,
                                 spec->location};
        sym = lowering_make_symbol(low, &sym_spec);
        lowering_scope_add(low, spec->name, sym);
    }
    TtNode *callee = lowering_make_var_ref(low, sym, spec->location);
    TtNode *node = tt_new(low->tt_arena, TT_CALL, spec->return_type, spec->location);
    node->call.callee = callee;
    node->call.arguments = spec->args;
    return node;
}
