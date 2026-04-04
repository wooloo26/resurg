#include "codegen/_codegen.h"

// ── Compound type collection ───────────────────────────────────────────

static const Type **g_compound_types = NULL; /* buf */

static bool compound_type_registered(const Type *type) {
    for (int32_t i = 0; i < BUFFER_LENGTH(g_compound_types); i++) {
        if (type_equal(g_compound_types[i], type)) {
            return true;
        }
    }
    return false;
}

static void register_compound_type(const Type *type) {
    if (type == NULL) {
        return;
    }
    if (type->kind == TYPE_ARRAY) {
        register_compound_type(type->array_element);
        if (!compound_type_registered(type)) {
            // NOLINTNEXTLINE(bugprone-sizeof-expression)
            BUFFER_PUSH(g_compound_types, type);
        }
    } else if (type->kind == TYPE_TUPLE) {
        for (int32_t i = 0; i < type->tuple_count; i++) {
            register_compound_type(type->tuple_elements[i]);
        }
        if (!compound_type_registered(type)) {
            // NOLINTNEXTLINE(bugprone-sizeof-expression)
            BUFFER_PUSH(g_compound_types, type);
        }
    }
}

static void collect_compound_types_recurse(const ASTNode *node);

/** Recurse over each element of a stretchy buffer of ASTNode pointers. */
static void recurse_children(ASTNode **children, int32_t count) {
    for (int32_t i = 0; i < count; i++) {
        collect_compound_types_recurse(children[i]);
    }
}

/** Recursively walk @p node collecting all array/tuple types. */
static void collect_compound_types_recurse(const ASTNode *node) {
    if (node == NULL) {
        return;
    }
    if (node->type != NULL) {
        register_compound_type(node->type);
    }
    switch (node->kind) {
    case NODE_FILE:
        recurse_children(node->file.declarations, BUFFER_LENGTH(node->file.declarations));
        break;
    case NODE_FUNCTION_DECLARATION:
        recurse_children(node->function_declaration.parameters,
                         BUFFER_LENGTH(node->function_declaration.parameters));
        collect_compound_types_recurse(node->function_declaration.body);
        break;
    case NODE_BLOCK:
        recurse_children(node->block.statements, BUFFER_LENGTH(node->block.statements));
        collect_compound_types_recurse(node->block.result);
        break;
    case NODE_VARIABLE_DECLARATION:
        collect_compound_types_recurse(node->variable_declaration.initializer);
        break;
    case NODE_EXPRESSION_STATEMENT:
        collect_compound_types_recurse(node->expression_statement.expression);
        break;
    case NODE_BINARY:
        collect_compound_types_recurse(node->binary.left);
        collect_compound_types_recurse(node->binary.right);
        break;
    case NODE_UNARY:
        collect_compound_types_recurse(node->unary.operand);
        break;
    case NODE_CALL:
        collect_compound_types_recurse(node->call.callee);
        recurse_children(node->call.arguments, BUFFER_LENGTH(node->call.arguments));
        break;
    case NODE_IF:
        collect_compound_types_recurse(node->if_expression.condition);
        collect_compound_types_recurse(node->if_expression.then_body);
        collect_compound_types_recurse(node->if_expression.else_body);
        break;
    case NODE_ASSIGN:
        collect_compound_types_recurse(node->assign.target);
        collect_compound_types_recurse(node->assign.value);
        break;
    case NODE_COMPOUND_ASSIGN:
        collect_compound_types_recurse(node->compound_assign.target);
        collect_compound_types_recurse(node->compound_assign.value);
        break;
    case NODE_ARRAY_LITERAL:
        recurse_children(node->array_literal.elements, BUFFER_LENGTH(node->array_literal.elements));
        break;
    case NODE_TUPLE_LITERAL:
        recurse_children(node->tuple_literal.elements, BUFFER_LENGTH(node->tuple_literal.elements));
        break;
    case NODE_INDEX:
        collect_compound_types_recurse(node->index_access.object);
        collect_compound_types_recurse(node->index_access.index);
        break;
    case NODE_TYPE_CONVERSION:
        collect_compound_types_recurse(node->type_conversion.operand);
        break;
    case NODE_MEMBER:
        collect_compound_types_recurse(node->member.object);
        break;
    case NODE_STRING_INTERPOLATION:
        recurse_children(node->string_interpolation.parts,
                         BUFFER_LENGTH(node->string_interpolation.parts));
        break;
    case NODE_LOOP:
        collect_compound_types_recurse(node->loop.body);
        break;
    case NODE_FOR:
        collect_compound_types_recurse(node->for_loop.start);
        collect_compound_types_recurse(node->for_loop.end);
        collect_compound_types_recurse(node->for_loop.body);
        break;
    default:
        break;
    }
}

void codegen_collect_compound_types(const ASTNode *node) {
    collect_compound_types_recurse(node);
}

void codegen_emit_compound_typedefs(CodeGenerator *generator) {
    for (int32_t i = 0; i < BUFFER_LENGTH(g_compound_types); i++) {
        const Type *type = g_compound_types[i];
        const char *name = codegen_c_type_for(generator, type);
        if (type->kind == TYPE_ARRAY) {
            codegen_emit_line(generator, "typedef struct { %s _data[%d]; } %s;",
                              codegen_c_type_for(generator, type->array_element), type->array_size,
                              name);
        } else if (type->kind == TYPE_TUPLE) {
            codegen_emit_indent(generator);
            fprintf(generator->output, "typedef struct {");
            for (int32_t j = 0; j < type->tuple_count; j++) {
                const char *elem_type = codegen_c_type_for(generator, type->tuple_elements[j]);
                fprintf(generator->output, " %s _%d;", elem_type, j);
            }
            fprintf(generator->output, " } %s;\n", name);
        }
    }
    if (BUFFER_LENGTH(g_compound_types) > 0) {
        codegen_emit(generator, "\n");
    }
}

void codegen_reset_compound_types(void) {
    if (g_compound_types != NULL) {
        BUFFER_FREE(g_compound_types);
        g_compound_types = NULL;
    }
}
