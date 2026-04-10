#ifndef RSG_MODULE_GRAPH_H
#define RSG_MODULE_GRAPH_H

#include "core/common.h"

/**
 * @file module_graph.h
 * @brief Module dependency graph — tracks parsed modules and their relationships.
 *
 * Caches parsed ASTs so that shared dependencies (e.g. std library modules)
 * are only parsed once.  Maintains forward and reverse dependency edges
 * for incremental recompilation and cycle detection.
 *
 * Usage:
 * @code
 *     ModuleGraph *graph = mod_graph_create(arena);
 *     ModuleNode *root = mod_graph_add(graph, "main.rsg", root_ast);
 *     ModuleNode *dep  = mod_graph_add(graph, "std/io.rsg", io_ast);
 *     mod_graph_add_dep(root, dep);
 *     if (!mod_graph_topo_sort(graph)) {
 *         // cycle detected
 *     }
 *     mod_graph_destroy(graph);
 * @endcode
 */

typedef struct ASTNode ASTNode;

// ── ModuleNode ─────────────────────────────────────────────────────────

typedef struct ModuleNode ModuleNode;

/** A node in the module dependency graph. */
struct ModuleNode {
    const char *path;   // canonical module path (arena-owned)
    ASTNode *ast;       // parsed AST (cached), or NULL if not yet parsed
    ModuleNode **deps;  /* buf — modules this module imports */
    ModuleNode **rdeps; /* buf — modules that import this module (reverse deps) */
    bool is_dirty;      // true when the module needs re-checking
    int32_t topo_idx;   // index in topological order (-1 before sorting)
};

// ── ModuleGraph ────────────────────────────────────────────────────────

typedef struct ModuleGraph ModuleGraph;

/** Create a module graph that allocates nodes from @p arena. */
ModuleGraph *mod_graph_create(Arena *arena);

/** Destroy the module graph (frees heap bufs; arena memory freed with arena). */
void mod_graph_destroy(ModuleGraph *graph);

/**
 * Add or retrieve a module node for @p path.
 *
 * If a node for @p path already exists, returns it (ignoring @p ast).
 * Otherwise, creates a new node with the given @p ast (may be NULL for
 * lazy parsing).
 */
ModuleNode *mod_graph_add(ModuleGraph *graph, const char *path, ASTNode *ast);

/** Look up a module node by @p path.  Returns NULL if not found. */
ModuleNode *mod_graph_lookup(const ModuleGraph *graph, const char *path);

/**
 * Record a dependency: @p from imports @p to.
 *
 * Adds @p to to @p from's dep list and @p from to @p to's rdep list.
 */
void mod_graph_add_dep(ModuleNode *from, ModuleNode *to);

/**
 * Compute a topological ordering of all modules.
 *
 * On success, returns true and populates each node's @c topo_idx.
 * On failure (cycle detected), returns false.  The cycle path can
 * be retrieved with mod_graph_cycle_path().
 */
bool mod_graph_topo_sort(ModuleGraph *graph);

/**
 * Retrieve the topologically sorted module list.
 *
 * Only valid after a successful mod_graph_topo_sort().  Returns a
 * stretchy buf (BUF_LEN for count).  The order is dependency-first:
 * if A depends on B, B appears before A.
 */
ModuleNode **mod_graph_topo_order(const ModuleGraph *graph);

/** Return the number of modules in the graph. */
int32_t mod_graph_count(const ModuleGraph *graph);

/**
 * Mark @p node dirty and propagate to all transitive reverse
 * dependents.  Returns the number of modules marked dirty.
 */
int32_t mod_graph_mark_dirty(ModuleNode *node);

/**
 * Collect all dirty modules in topological order.
 *
 * Returns a heap-allocated stretchy buf containing only dirty modules
 * sorted by their @c topo_idx.  The caller must BUF_FREE the result.
 * Only valid after a successful mod_graph_topo_sort().
 */
ModuleNode **mod_graph_dirty_set(const ModuleGraph *graph);

#endif // RSG_MODULE_GRAPH_H
