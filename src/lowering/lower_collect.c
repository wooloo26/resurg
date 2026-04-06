#include "_lowering.h"

// ── Compound type collection ──────────────────────────────────────────

/** Return true if @p type is already in the compound type registry. */
static bool is_compound_type_registered(const Lowering *low, const Type *type) {
    for (int32_t i = 0; i < BUF_LEN(low->compound_types); i++) {
        if (type_equal(low->compound_types[i], type)) {
            return true;
        }
    }
    return false;
}

/** Register an array, tuple, or slice type (and its children) in the lowering registry. */
static void register_compound_type(Lowering *low, const Type *type) {
    if (type == NULL) {
        return;
    }
    if (type->kind == TYPE_ARRAY) {
        register_compound_type(low, type->array.elem);
    } else if (type->kind == TYPE_SLICE) {
        register_compound_type(low, type->slice.elem);
        return; // slices use the generic RsgSlice type, no per-type typedef
    } else if (type->kind == TYPE_TUPLE) {
        for (int32_t i = 0; i < type->tuple.count; i++) {
            register_compound_type(low, type->tuple.elems[i]);
        }
    } else if (type->kind == TYPE_STRUCT) {
        for (int32_t i = 0; i < type->struct_type.embed_count; i++) {
            register_compound_type(low, type->struct_type.embedded[i]);
        }
        for (int32_t i = 0; i < type->struct_type.field_count; i++) {
            register_compound_type(low, type->struct_type.fields[i].type);
        }
    } else if (type->kind == TYPE_PTR) {
        register_compound_type(low, type->ptr.pointee);
        return;
    } else {
        return;
    }
    if (!is_compound_type_registered(low, type)) {
        BUF_PUSH(low->compound_types, type);
    }
}

static void collect_compound_types(Lowering *low, const TTNode *node);

/** Visitor adapter: collect compound types from each child. */
static void collect_visitor(void *ctx, TTNode **child_ptr) {
    collect_compound_types((Lowering *)ctx, *child_ptr);
}

/** Walk @p node collecting all array/tuple types into the lowering registry. */
static void collect_compound_types(Lowering *low, const TTNode *node) {
    if (node == NULL) {
        return;
    }
    if (node->type != NULL) {
        register_compound_type(low, node->type);
    }
    tt_visit_children((TTNode *)node, collect_visitor, low);
}

// ── Public API ─────────────────────────────────────────────────────────

Lowering *lowering_create(Arena *tt_arena) {
    Lowering *low = rsg_malloc(sizeof(Lowering));
    low->tt_arena = tt_arena;
    low->scope = NULL;
    low->err_count = 0;
    low->current_module = NULL;
    low->temp_counter = 0;
    low->shadow_counter = 0;
    low->compound_types = NULL;
    low->current_recv = NULL;
    low->current_recv_name = NULL;
    low->current_is_ptr_recv = false;
    return low;
}

void lowering_destroy(Lowering *lowering) {
    if (lowering == NULL) {
        return;
    }
    BUF_FREE(lowering->compound_types);
    free(lowering);
}

TTNode *lowering_lower(Lowering *lowering, const ASTNode *file) {
    TTNode *tt_file = lower_node(lowering, file);
    collect_compound_types(lowering, tt_file);
    tt_file->file.compound_types = lowering->compound_types;
    return tt_file;
}
