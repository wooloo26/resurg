#include "_sema.h"

// ── Scope manipulation ─────────────────────────────────────────────────

Scope *scope_push(Sema *analyzer, bool is_loop) {
    Scope *scope = arena_alloc(analyzer->arena, sizeof(Scope));
    hash_table_init(&scope->table, analyzer->arena);
    scope->parent = analyzer->current_scope;
    scope->is_loop = is_loop;
    scope->module_name =
        analyzer->current_scope != NULL ? analyzer->current_scope->module_name : NULL;
    analyzer->current_scope = scope;
    return scope;
}

void scope_pop(Sema *analyzer) {
    analyzer->current_scope = analyzer->current_scope->parent;
}

void scope_define(Sema *analyzer, const SymDef *def) {
    Sym *sym = arena_alloc(analyzer->arena, sizeof(Sym));
    sym->name = def->name;
    sym->type = def->type;
    sym->kind = def->kind;
    sym->is_pub = def->is_pub;
    sym->is_immut = false;
    sym->decl = NULL;
    sym->owner = NULL;
    hash_table_insert(&analyzer->current_scope->table, def->name, sym);
}

Sym *scope_lookup_current(const Sema *analyzer, const char *name) {
    return hash_table_lookup(&analyzer->current_scope->table, name);
}

Sym *scope_lookup(const Sema *analyzer, const char *name) {
    for (Scope *scope = analyzer->current_scope; scope != NULL; scope = scope->parent) {
        Sym *sym = hash_table_lookup(&scope->table, name);
        if (sym != NULL) {
            return sym;
        }
    }
    return NULL;
}

bool in_loop(const Sema *analyzer) {
    for (Scope *scope = analyzer->current_scope; scope != NULL; scope = scope->parent) {
        if (scope->is_loop) {
            return true;
        }
    }
    return false;
}
