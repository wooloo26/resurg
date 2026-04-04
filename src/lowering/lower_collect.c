#include "_lowering.h"

// ── Compound type collection ──────────────────────────────────────────

/** Return true if @p type is already in the compound type registry. */
static bool compound_type_registered(const Lowering *low, const Type *type) {
    for (int32_t i = 0; i < BUFFER_LENGTH(low->compound_types); i++) {
        if (type_equal(low->compound_types[i], type)) {
            return true;
        }
    }
    return false;
}

/** Register an array or tuple type (and its children) in the lowering registry. */
static void register_compound_type(Lowering *low, const Type *type) {
    if (type == NULL) {
        return;
    }
    if (type->kind == TYPE_ARRAY) {
        register_compound_type(low, type->array.element);
    } else if (type->kind == TYPE_TUPLE) {
        for (int32_t i = 0; i < type->tuple.count; i++) {
            register_compound_type(low, type->tuple.elements[i]);
        }
    } else {
        return;
    }
    if (!compound_type_registered(low, type)) {
        BUFFER_PUSH(low->compound_types, type);
    }
}

static void collect_compound_types(Lowering *low, const TtNode *node);

/** Visitor adapter: collect compound types from each child. */
static void collect_visitor(void *ctx, TtNode **child_ptr) {
    collect_compound_types((Lowering *)ctx, *child_ptr);
}

/** Walk @p node collecting all array/tuple types into the lowering registry. */
static void collect_compound_types(Lowering *low, const TtNode *node) {
    if (node == NULL) {
        return;
    }
    if (node->type != NULL) {
        register_compound_type(low, node->type);
    }
    tt_visit_children((TtNode *)node, collect_visitor, low);
}

// ── Public API ─────────────────────────────────────────────────────────

Lowering *lowering_create(Arena *tt_arena, Arena *sema_arena) {
    Lowering *low = rsg_malloc(sizeof(Lowering));
    low->tt_arena = tt_arena;
    low->sema_arena = sema_arena;
    low->scope = NULL;
    low->error_count = 0;
    low->current_module = NULL;
    low->temp_counter = 0;
    low->shadow_counter = 0;
    low->compound_types = NULL;
    return low;
}

void lowering_destroy(Lowering *lowering) {
    BUFFER_FREE(lowering->compound_types);
    free(lowering);
}

TtNode *lowering_lower(Lowering *lowering, const ASTNode *file) {
    TtNode *tt_file = lower_node(lowering, file);
    collect_compound_types(lowering, tt_file);
    tt_file->file.compound_types = lowering->compound_types;
    return tt_file;
}
