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

TtSymbol *lowering_make_symbol(Lowering *low, TtSymbolKind kind, const char *name, const Type *type,
                               bool is_mut, SourceLocation location) {
    Symbol *sema_sym = arena_alloc_zero(low->tt_arena, sizeof(Symbol));
    sema_sym->name = name;
    sema_sym->type = type;
    switch (kind) {
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
    return tt_symbol_new(low->tt_arena, kind, sema_sym, is_mut, location);
}

TtSymbol *lowering_add_variable(Lowering *low, const char *name, const Type *type, bool is_mut,
                                SourceLocation location) {
    TtSymbol *sym = lowering_make_symbol(low, TT_SYMBOL_VARIABLE, name, type, is_mut, location);
    lowering_scope_add(low, name, sym);
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

TtNode *lowering_make_int_lit(Lowering *low, uint64_t value, const Type *type, TypeKind int_kind,
                              SourceLocation location) {
    TtNode *node = tt_new(low->tt_arena, TT_INT_LITERAL, type, location);
    node->int_literal.value = value;
    node->int_literal.int_kind = int_kind;
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

TtNode *lowering_make_builtin_call(Lowering *low, const char *name, const Type *return_type,
                                   TtNode **args, SourceLocation location) {
    TtSymbol *sym = lowering_scope_find(low, name);
    if (sym == NULL) {
        sym = lowering_make_symbol(low, TT_SYMBOL_FUNCTION, name, return_type, false, location);
        lowering_scope_add(low, name, sym);
    }
    TtNode *callee = lowering_make_var_ref(low, sym, location);
    TtNode *node = tt_new(low->tt_arena, TT_CALL, return_type, location);
    node->call.callee = callee;
    node->call.arguments = args;
    return node;
}
