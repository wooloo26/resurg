#include "core/module_graph.h"

/**
 * @file module_graph.c
 * @brief Module dependency graph implementation.
 */

struct ModuleGraph {
    Arena *arena;            // arena for ModuleNode allocations
    HashTable modules;       // path → ModuleNode*
    ModuleNode **all_nodes;  /* buf — all nodes in insertion order */
    ModuleNode **topo_order; /* buf — topologically sorted nodes */
    bool sorted;             // true after successful topo_sort
};

// ── Lifecycle ──────────────────────────────────────────────────────────

ModuleGraph *mod_graph_create(Arena *arena) {
    ModuleGraph *graph = arena_alloc_zero(arena, sizeof(*graph));
    graph->arena = arena;
    hash_table_init(&graph->modules, arena);
    graph->all_nodes = NULL;
    graph->topo_order = NULL;
    graph->sorted = false;
    return graph;
}

void mod_graph_destroy(ModuleGraph *graph) {
    if (graph == NULL) {
        return;
    }
    // Free heap-owned stretchy bufs (dep/rdep lists in each node).
    for (int32_t i = 0; i < BUF_LEN(graph->all_nodes); i++) {
        ModuleNode *node = graph->all_nodes[i];
        BUF_FREE(node->deps);
        BUF_FREE(node->rdeps);
    }
    BUF_FREE(graph->all_nodes);
    BUF_FREE(graph->topo_order);
    // hash table and ModuleNode bodies are arena-allocated.
}

// ── Node management ────────────────────────────────────────────────────

ModuleNode *mod_graph_add(ModuleGraph *graph, const char *path, ASTNode *ast) {
    ModuleNode *existing = hash_table_lookup(&graph->modules, path);
    if (existing != NULL) {
        return existing;
    }

    ModuleNode *node = arena_alloc_zero(graph->arena, sizeof(*node));
    node->path = arena_strdup(graph->arena, path);
    node->ast = ast;
    node->deps = NULL;
    node->rdeps = NULL;
    node->is_dirty = false;
    node->topo_idx = -1;

    hash_table_insert(&graph->modules, node->path, node);
    BUF_PUSH(graph->all_nodes, node);
    graph->sorted = false;
    return node;
}

ModuleNode *mod_graph_lookup(const ModuleGraph *graph, const char *path) {
    return hash_table_lookup(&graph->modules, path);
}

int32_t mod_graph_count(const ModuleGraph *graph) {
    return BUF_LEN(graph->all_nodes);
}

// ── Dependency edges ───────────────────────────────────────────────────

/** Check if @p dep is already in @p node's dep list. */
static bool has_dep(const ModuleNode *node, const ModuleNode *dep) {
    for (int32_t i = 0; i < BUF_LEN(node->deps); i++) {
        if (node->deps[i] == dep) {
            return true;
        }
    }
    return false;
}

void mod_graph_add_dep(ModuleNode *from, ModuleNode *to) {
    if (from == to || has_dep(from, to)) {
        return;
    }
    BUF_PUSH(from->deps, to);
    BUF_PUSH(to->rdeps, from);
}

// ── Topological sort (Kahn's algorithm) ────────────────────────────────

bool mod_graph_topo_sort(ModuleGraph *graph) {
    int32_t n = BUF_LEN(graph->all_nodes);
    if (n == 0) {
        graph->sorted = true;
        return true;
    }

    // Compute in-degrees.
    int32_t *in_degree = rsg_calloc((size_t)n, sizeof(int32_t));

    // Build index mapping: node → position in all_nodes.
    // Use a temporary array since nodes are arena-allocated.
    for (int32_t i = 0; i < n; i++) {
        graph->all_nodes[i]->topo_idx = i; // temporary: store position
    }
    for (int32_t i = 0; i < n; i++) {
        ModuleNode *node = graph->all_nodes[i];
        for (int32_t j = 0; j < BUF_LEN(node->deps); j++) {
            (void)node; // in_degree of deps
            in_degree[node->deps[j]->topo_idx]++;
        }
    }

    // Seed queue with zero-in-degree nodes.
    int32_t *queue = rsg_malloc((size_t)n * sizeof(int32_t));
    int32_t q_head = 0;
    int32_t q_tail = 0;
    for (int32_t i = 0; i < n; i++) {
        if (in_degree[i] == 0) {
            queue[q_tail++] = i;
        }
    }

    // Process queue.
    BUF_FREE(graph->topo_order);
    graph->topo_order = NULL;

    while (q_head < q_tail) {
        int32_t idx = queue[q_head++];
        ModuleNode *node = graph->all_nodes[idx];
        BUF_PUSH(graph->topo_order, node);

        // For topo sort: edges are from importers to importees.
        // Since we want dependency-first, we need importees (deps) before importers.
        // Kahn's: reduce in-degree of nodes that import this one (rdeps).
        for (int32_t j = 0; j < BUF_LEN(node->rdeps); j++) {
            int32_t dep_idx = node->rdeps[j]->topo_idx;
            in_degree[dep_idx]--;
            if (in_degree[dep_idx] == 0) {
                queue[q_tail++] = dep_idx;
            }
        }
    }

    free(in_degree);
    free(queue);

    bool has_cycle = BUF_LEN(graph->topo_order) != n;
    if (has_cycle) {
        BUF_FREE(graph->topo_order);
        graph->sorted = false;
        return false;
    }

    // Update topo_idx to reflect final ordering.
    for (int32_t i = 0; i < n; i++) {
        graph->topo_order[i]->topo_idx = i;
    }
    graph->sorted = true;
    return true;
}

ModuleNode **mod_graph_topo_order(const ModuleGraph *graph) {
    return graph->topo_order;
}

// ── Dirty propagation ──────────────────────────────────────────────────

/** Recursive dirty propagation through reverse deps. */
static int32_t mark_dirty_recursive(ModuleNode *node) {
    if (node->is_dirty) {
        return 0;
    }
    node->is_dirty = true;
    int32_t count = 1;
    for (int32_t i = 0; i < BUF_LEN(node->rdeps); i++) {
        count += mark_dirty_recursive(node->rdeps[i]);
    }
    return count;
}

int32_t mod_graph_mark_dirty(ModuleNode *node) {
    return mark_dirty_recursive(node);
}

ModuleNode **mod_graph_dirty_set(const ModuleGraph *graph) {
    ModuleNode **dirty = NULL;
    if (!graph->sorted || graph->topo_order == NULL) {
        return dirty;
    }
    for (int32_t i = 0; i < BUF_LEN(graph->topo_order); i++) {
        ModuleNode *node = graph->topo_order[i];
        if (node->is_dirty) {
            BUF_PUSH(dirty, node);
        }
    }
    return dirty;
}
