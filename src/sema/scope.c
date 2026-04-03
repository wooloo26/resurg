#include "sema/_sema.h"

// ── Scope manipulation ─────────────────────────────────────────────────

Scope *scope_push(SemanticAnalyzer *analyzer, bool is_loop) {
    Scope *scope = arena_alloc(analyzer->arena, sizeof(Scope));
    scope->symbols = NULL;
    scope->parent = analyzer->current_scope;
    scope->is_loop = is_loop;
    scope->module_name = analyzer->current_scope != NULL ? analyzer->current_scope->module_name : NULL;
    analyzer->current_scope = scope;
    return scope;
}

void scope_pop(SemanticAnalyzer *analyzer) {
    analyzer->current_scope = analyzer->current_scope->parent;
}

void scope_define(SemanticAnalyzer *analyzer, const char *name, const Type *type, bool is_public, bool is_function) {
    Symbol *symbol = arena_alloc(analyzer->arena, sizeof(Symbol));
    symbol->name = name;
    symbol->type = type;
    symbol->is_public = is_public;
    symbol->is_function = is_function;
    symbol->next = analyzer->current_scope->symbols;
    analyzer->current_scope->symbols = symbol;
}

Symbol *scope_lookup_current(const SemanticAnalyzer *analyzer, const char *name) {
    for (Symbol *symbol = analyzer->current_scope->symbols; symbol != NULL; symbol = symbol->next) {
        if (strcmp(symbol->name, name) == 0) {
            return symbol;
        }
    }
    return NULL;
}

Symbol *scope_lookup(const SemanticAnalyzer *analyzer, const char *name) {
    for (Scope *scope = analyzer->current_scope; scope != NULL; scope = scope->parent) {
        for (Symbol *symbol = scope->symbols; symbol != NULL; symbol = symbol->next) {
            if (strcmp(symbol->name, name) == 0) {
                return symbol;
            }
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
