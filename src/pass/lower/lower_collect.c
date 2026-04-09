#include "_lower.h"

// ── Compound type collection ──────────────────────────────────────────

/** Return true if @p type is already in the compound type registry. */
static bool is_compound_type_registered(const Lower *low, const Type *type) {
    for (int32_t i = 0; i < BUF_LEN(low->compound_types); i++) {
        if (type_equal(low->compound_types[i], type)) {
            return true;
        }
    }
    return false;
}

/** Register an array, tuple, or slice type (and its children) in the lower registry. */
static void register_compound_type(Lower *low, const Type *type) {
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

static void collect_compound_types(Lower *low, const HirNode *node);

/** Visitor adapter: collect compound types from each child. */
static void collect_visitor(void *ctx, HirNode **child_ptr) {
    collect_compound_types((Lower *)ctx, *child_ptr);
}

/** Walk @p node collecting all array/tuple types into the lower registry. */
static void collect_compound_types(Lower *low, const HirNode *node) {
    if (node == NULL) {
        return;
    }
    if (node->type != NULL) {
        register_compound_type(low, node->type);
    }
    hir_visit_children((HirNode *)node, collect_visitor, low);
}

// ── Public API ─────────────────────────────────────────────────────────

Lower *lower_create(Arena *hir_arena) {
    Lower *low = rsg_malloc(sizeof(Lower));
    low->hir_arena = hir_arena;
    low->scope = NULL;
    low->err_count = 0;
    low->current_module = NULL;
    low->temp_counter = 0;
    low->shadow_counter = 0;
    low->compound_types = NULL;
    low->recv = (RecvCtx){0};
    low->fn_return_type = NULL;
    low->closure_counter = 0;
    return low;
}

void lower_destroy(Lower *lower) {
    if (lower == NULL) {
        return;
    }
    BUF_FREE(lower->compound_types);
    free(lower);
}

HirNode *lower_lower(Lower *lower, const ASTNode *file) {
    HirNode *hir_file = lower_node(lower, file);
    collect_compound_types(lower, hir_file);
    hir_file->file.compound_types = lower->compound_types;
    return hir_file;
}
