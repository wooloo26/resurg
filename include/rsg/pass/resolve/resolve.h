#ifndef RSG_PUB_RESOLVE_H
#define RSG_PUB_RESOLVE_H

#include "repr/ast.h"

/**
 * @file resolve.h
 * @brief Resolve pass — name resolution and declaration registration.
 *
 * Creates the shared semantic context (Sema) and populates it with
 * all declared symbols: pacts, structs, enums, type aliases, fn sigs.
 * Must run before the check and mono passes.
 */
typedef struct Sema Sema;

/** Callback to load module decls from a file path. Returns NULL on failure. */
typedef ASTNode **(*ModuleLoader)(void *ctx, Arena *arena, const char *mod_path);

/** Create a semantic context that allocates auxiliary data from @p arena. */
Sema *sema_create(Arena *arena);
/** Destroy the context (does not free the arena). */
void sema_destroy(Sema *sema);
/** Inject a module loader callback used during resolve to load filesystem modules. */
void sema_set_module_loader(Sema *sema, ModuleLoader loader, void *ctx);
/**
 * Resolve pass: push global scope and register all declarations in @p file.
 * Returns true when no errs were found.
 */
bool sema_resolve(Sema *sema, ASTNode *file);

#endif // RSG_PUB_RESOLVE_H
