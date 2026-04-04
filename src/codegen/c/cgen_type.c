#include "_codegen.h"

// ── Compound type collection ───────────────────────────────────────────

static bool compound_type_registered(const CodeGenerator *generator, const Type *type) {
    for (int32_t i = 0; i < BUFFER_LENGTH(generator->compound_types); i++) {
        if (type_equal(generator->compound_types[i], type)) {
            return true;
        }
    }
    return false;
}

static void register_compound_type(CodeGenerator *generator, const Type *type) {
    if (type == NULL) {
        return;
    }
    if (type->kind == TYPE_ARRAY) {
        register_compound_type(generator, type->array.element);
        if (!compound_type_registered(generator, type)) {
            BUFFER_PUSH(generator->compound_types, type);
        }
    } else if (type->kind == TYPE_TUPLE) {
        for (int32_t i = 0; i < type->tuple.count; i++) {
            register_compound_type(generator, type->tuple.elements[i]);
        }
        if (!compound_type_registered(generator, type)) {
            BUFFER_PUSH(generator->compound_types, type);
        }
    }
}

static void collect_compound_types_recurse(CodeGenerator *generator, const TtNode *node);

/** Recurse over each element of a stretchy buffer of TtNode pointers. */
static void recurse_children(CodeGenerator *generator, TtNode **children, int32_t count) {
    for (int32_t i = 0; i < count; i++) {
        collect_compound_types_recurse(generator, children[i]);
    }
}

/** Recursively walk @p node collecting all array/tuple types. */
static void collect_compound_types_recurse(CodeGenerator *generator, const TtNode *node) {
    if (node == NULL) {
        return;
    }
    if (node->type != NULL) {
        register_compound_type(generator, node->type);
    }
    switch (node->kind) {
    case TT_FILE:
        recurse_children(generator, node->file.declarations,
                         BUFFER_LENGTH(node->file.declarations));
        break;
    case TT_FUNCTION_DECLARATION:
        recurse_children(generator, node->function_declaration.params,
                         BUFFER_LENGTH(node->function_declaration.params));
        collect_compound_types_recurse(generator, node->function_declaration.body);
        break;
    case TT_BLOCK:
        recurse_children(generator, node->block.statements, BUFFER_LENGTH(node->block.statements));
        collect_compound_types_recurse(generator, node->block.result);
        break;
    case TT_VARIABLE_DECLARATION:
        collect_compound_types_recurse(generator, node->variable_declaration.initializer);
        break;
    case TT_EXPRESSION_STATEMENT:
        collect_compound_types_recurse(generator, node->expression_statement.expression);
        break;
    case TT_RETURN:
        collect_compound_types_recurse(generator, node->return_statement.value);
        break;
    case TT_BINARY:
        collect_compound_types_recurse(generator, node->binary.left);
        collect_compound_types_recurse(generator, node->binary.right);
        break;
    case TT_UNARY:
        collect_compound_types_recurse(generator, node->unary.operand);
        break;
    case TT_CALL:
        collect_compound_types_recurse(generator, node->call.callee);
        recurse_children(generator, node->call.arguments, BUFFER_LENGTH(node->call.arguments));
        break;
    case TT_IF:
        collect_compound_types_recurse(generator, node->if_expression.condition);
        collect_compound_types_recurse(generator, node->if_expression.then_body);
        collect_compound_types_recurse(generator, node->if_expression.else_body);
        break;
    case TT_ASSIGN:
        collect_compound_types_recurse(generator, node->assign.target);
        collect_compound_types_recurse(generator, node->assign.value);
        break;
    case TT_ARRAY_LITERAL:
        recurse_children(generator, node->array_literal.elements,
                         BUFFER_LENGTH(node->array_literal.elements));
        break;
    case TT_TUPLE_LITERAL:
        recurse_children(generator, node->tuple_literal.elements,
                         BUFFER_LENGTH(node->tuple_literal.elements));
        break;
    case TT_INDEX:
        collect_compound_types_recurse(generator, node->index_access.object);
        collect_compound_types_recurse(generator, node->index_access.index);
        break;
    case TT_TUPLE_INDEX:
        collect_compound_types_recurse(generator, node->tuple_index.object);
        break;
    case TT_TYPE_CONVERSION:
        collect_compound_types_recurse(generator, node->type_conversion.operand);
        break;
    case TT_MODULE_ACCESS:
        collect_compound_types_recurse(generator, node->module_access.object);
        break;
    case TT_LOOP:
        collect_compound_types_recurse(generator, node->loop.body);
        break;
    default:
        break;
    }
}

void codegen_collect_compound_types(CodeGenerator *generator, const TtNode *node) {
    collect_compound_types_recurse(generator, node);
}

void codegen_emit_compound_typedefs(CodeGenerator *generator) {
    for (int32_t i = 0; i < BUFFER_LENGTH(generator->compound_types); i++) {
        const Type *type = generator->compound_types[i];
        const char *name = codegen_c_type_for(generator, type);
        if (type->kind == TYPE_ARRAY) {
            codegen_emit_line(generator, "typedef struct { %s _data[%d]; } %s;",
                              codegen_c_type_for(generator, type->array.element), type->array.size,
                              name);
        } else if (type->kind == TYPE_TUPLE) {
            codegen_emit_indent(generator);
            fprintf(generator->output, "typedef struct {");
            for (int32_t j = 0; j < type->tuple.count; j++) {
                const char *elem_type = codegen_c_type_for(generator, type->tuple.elements[j]);
                fprintf(generator->output, " %s _%d;", elem_type, j);
            }
            fprintf(generator->output, " } %s;\n", name);
        }
    }
    if (BUFFER_LENGTH(generator->compound_types) > 0) {
        codegen_emit(generator, "\n");
    }
}

void codegen_reset_compound_types(CodeGenerator *generator) {
    if (generator->compound_types != NULL) {
        BUFFER_FREE(generator->compound_types);
        generator->compound_types = NULL;
    }
}
