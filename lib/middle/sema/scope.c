#include "_sema.h"

// ── Scope manipulation ─────────────────────────────────────────────────

Scope *scope_push(SemanticAnalyzer *analyzer, bool is_loop) {
    Scope *scope = arena_alloc(analyzer->arena, sizeof(Scope));
    hash_table_init(&scope->table, analyzer->arena);
    scope->parent = analyzer->current_scope;
    scope->is_loop = is_loop;
    scope->module_name =
        analyzer->current_scope != NULL ? analyzer->current_scope->module_name : NULL;
    analyzer->current_scope = scope;
    return scope;
}

void scope_pop(SemanticAnalyzer *analyzer) {
    analyzer->current_scope = analyzer->current_scope->parent;
}

void scope_define(SemanticAnalyzer *analyzer, const char *name, const Type *type, bool is_public,
                  SymbolKind kind) {
    Symbol *symbol = arena_alloc(analyzer->arena, sizeof(Symbol));
    symbol->name = name;
    symbol->type = type;
    symbol->kind = kind;
    symbol->is_public = is_public;
    symbol->is_immut = false;
    symbol->declaration = NULL;
    symbol->owner = NULL;
    hash_table_insert(&analyzer->current_scope->table, name, symbol);
}

Symbol *scope_lookup_current(const SemanticAnalyzer *analyzer, const char *name) {
    return hash_table_lookup(&analyzer->current_scope->table, name);
}

Symbol *scope_lookup(const SemanticAnalyzer *analyzer, const char *name) {
    for (Scope *scope = analyzer->current_scope; scope != NULL; scope = scope->parent) {
        Symbol *symbol = hash_table_lookup(&scope->table, name);
        if (symbol != NULL) {
            return symbol;
        }
    }
    return NULL;
}

bool in_loop(const SemanticAnalyzer *analyzer) {
    for (Scope *scope = analyzer->current_scope; scope != NULL; scope = scope->parent) {
        if (scope->is_loop) {
            return true;
        }
    }
    return false;
}
