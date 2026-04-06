#include "pass/check/_check.h"

// ── Scope manipulation ─────────────────────────────────────────────────

Scope *scope_push(Sema *sema, bool is_loop) {
    Scope *scope = arena_alloc(sema->arena, sizeof(Scope));
    hash_table_init(&scope->table, sema->arena);
    scope->parent = sema->current_scope;
    scope->is_loop = is_loop;
    scope->module_name = sema->current_scope != NULL ? sema->current_scope->module_name : NULL;
    sema->current_scope = scope;
    return scope;
}

void scope_pop(Sema *sema) {
    sema->current_scope = sema->current_scope->parent;
}

void scope_define(Sema *sema, const SymDef *def) {
    Sym *sym = arena_alloc(sema->arena, sizeof(Sym));
    sym->name = def->name;
    sym->type = def->type;
    sym->kind = def->kind;
    sym->is_pub = def->is_pub;
    sym->is_immut = false;
    sym->decl = NULL;
    sym->owner = NULL;
    hash_table_insert(&sema->current_scope->table, def->name, sym);
}

Sym *scope_lookup_current(const Sema *sema, const char *name) {
    return hash_table_lookup(&sema->current_scope->table, name);
}

Sym *scope_lookup(const Sema *sema, const char *name) {
    for (Scope *scope = sema->current_scope; scope != NULL; scope = scope->parent) {
        Sym *sym = hash_table_lookup(&scope->table, name);
        if (sym != NULL) {
            return sym;
        }
    }
    return NULL;
}

bool in_loop(const Sema *sema) {
    for (Scope *scope = sema->current_scope; scope != NULL; scope = scope->parent) {
        if (scope->is_loop) {
            return true;
        }
    }
    return false;
}
