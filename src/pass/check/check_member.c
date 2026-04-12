#include "_check.h"

// ── Member access ──────────────────────────────────────────────────────

/** Resolve a module-qualified member: mod::item. */
static const Type *check_module_member(Sema *sema, ASTNode *node, const Type *module_type) {
    const char *mod_name = module_type->module_type.name;
    const char *member = node->member.member;
    const char *qualified;
    if (strlen(mod_name) == 0) {
        qualified = member;
    } else {
        qualified = arena_sprintf(sema->base.arena, "%s.%s", mod_name, member);
    }

    // Try struct: mod::StructName
    StructDef *sdef = sema_lookup_struct(sema, qualified);
    if (sdef != NULL) {
        node->kind = NODE_ID;
        node->id.name = qualified;
        return sdef->type;
    }

    // Try enum: mod::EnumName
    EnumDef *edef = sema_lookup_enum(sema, qualified);
    if (edef != NULL) {
        node->kind = NODE_ID;
        node->id.name = qualified;
        return edef->type;
    }

    // Try type alias: mod::TypeName
    const Type *talias = sema_lookup_type_alias(sema, qualified);
    if (talias != NULL) {
        return talias;
    }

    // Try sub-module: mod::inner
    Sym *sub = scope_lookup(sema, qualified);
    if (sub != NULL && sub->type != NULL && sub->type->kind == TYPE_MODULE) {
        return sub->type;
    }

    // Try fn (for fn-as-value): mod::fn_name
    FnSig *sig = sema_lookup_fn(sema, qualified);
    if (sig != NULL) {
        node->kind = NODE_ID;
        node->id.name = qualified;
        const Type **params = NULL;
        for (int32_t i = 0; i < sig->param_count; i++) {
            BUF_PUSH(params, sig->param_types[i]);
        }
        FnTypeSpec fn_spec = {params, sig->param_count, sig->return_type, FN_PLAIN};
        return type_create_fn(sema->base.arena, &fn_spec);
    }

    SEMA_ERR(sema, node->loc, "'%s' not found in module '%s'", member, mod_name);
    return &TYPE_ERR_INST;
}

/** Resolve a struct field or promoted field access. */
static const Type *check_struct_member(Sema *sema, ASTNode *node, const Type *struct_type) {
    const char *field_name = node->member.member;

    // Tuple struct index: .0, .1, ... → look up field _0, _1, ...
    char *end = NULL;
    long idx = strtol(field_name, &end, 10);
    if (end != NULL && *end == '\0' && idx >= 0) {
        const char *synth_name = arena_sprintf(sema->base.arena, "_%ld", idx);
        const StructField *sf = type_struct_find_field(struct_type, synth_name);
        if (sf != NULL) {
            node->member.member = synth_name;
            return sf->type;
        }
    }

    // Check own fields (including embedded struct fields by name, e.g., e.Base)
    const StructField *sf = type_struct_find_field(struct_type, field_name);
    if (sf != NULL) {
        if (!sf->is_pub && is_foreign_struct(sema, struct_type->struct_type.name)) {
            SEMA_ERR(sema, node->loc, "field '%s' is private in struct '%s'", field_name,
                     struct_type->struct_type.name);
            return &TYPE_ERR_INST;
        }
        return sf->type;
    }

    // Check promoted fields from embedded structs
    StructDef *sdef = sema_lookup_struct(sema, struct_type->struct_type.name);
    if (sdef != NULL) {
        const Type *promoted = find_promoted_field(sema, sdef, field_name);
        if (promoted != NULL) {
            return promoted;
        }
    }

    SEMA_ERR(sema, node->loc, "no field '%s' on type '%s'", field_name,
             struct_type->struct_type.name);
    return &TYPE_ERR_INST;
}

const Type *check_member(Sema *sema, ASTNode *node) {
    const Type *object_type = check_node(sema, node->member.object);

    // Auto-deref: if object is a ptr, unwrap to pointee
    if (object_type != NULL && object_type->kind == TYPE_PTR) {
        object_type = object_type->ptr.pointee;
    }

    // Module member access: mod::item
    if (object_type != NULL && object_type->kind == TYPE_MODULE) {
        return check_module_member(sema, node, object_type);
    }

    // Tuple field access: .0, .1, .2, ...
    if (object_type != NULL && object_type->kind == TYPE_TUPLE) {
        char *end = NULL;
        long idx = strtol(node->member.member, &end, 10);
        if (end != NULL && *end == '\0' && idx >= 0 && idx < object_type->tuple.count) {
            return object_type->tuple.elems[idx];
        }
    }

    // Struct field access
    if (object_type != NULL && object_type->kind == TYPE_STRUCT) {
        return check_struct_member(sema, node, object_type);
    }

    // Enum variant access: EnumType.Variant
    if (object_type != NULL && object_type->kind == TYPE_ENUM) {
        const EnumVariant *variant = type_enum_find_variant(object_type, node->member.member);
        if (variant != NULL) {
            return object_type;
        }
        SEMA_ERR(sema, node->loc, "no variant '%s' on enum '%s'", node->member.member,
                 type_enum_name(object_type));
        return &TYPE_ERR_INST;
    }

    return &TYPE_ERR_INST;
}

const Type *check_idx(Sema *sema, ASTNode *node) {
    const Type *object_type = check_node(sema, node->idx_access.object);
    check_node(sema, node->idx_access.idx);
    // Auto-deref: unwrap *[]T / *[N]T
    if (object_type != NULL && object_type->kind == TYPE_PTR) {
        object_type = object_type->ptr.pointee;
    }
    if (object_type != NULL && object_type->kind == TYPE_ARRAY) {
        return object_type->array.elem;
    }
    if (object_type != NULL && object_type->kind == TYPE_SLICE) {
        return object_type->slice.elem;
    }
    return &TYPE_ERR_INST;
}
