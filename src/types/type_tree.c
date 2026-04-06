#include "types/type_tree.h"

#include "sema/_sema.h"

// ── TtSymbol ───────────────────────────────────────────────────────────

const char *tt_symbol_name(const TtSymbol *symbol) {
    assert(symbol != NULL && symbol->sema_symbol != NULL);
    return symbol->sema_symbol->name;
}

const Type *tt_symbol_type(const TtSymbol *symbol) {
    assert(symbol != NULL && symbol->sema_symbol != NULL);
    return symbol->sema_symbol->type;
}

TtSymbol *tt_symbol_new(Arena *arena, TtSymbolKind kind, Symbol *sema_symbol, bool is_mut,
                        SourceLocation location) {
    TtSymbol *symbol = arena_alloc_zero(arena, sizeof(TtSymbol));
    symbol->kind = kind;
    symbol->sema_symbol = sema_symbol;
    symbol->is_mut = is_mut;
    symbol->location = location;
    return symbol;
}

// ── TtNode constructors ───────────────────────────────────────────────

TtNode *tt_new(Arena *arena, TtNodeKind kind, const Type *type, SourceLocation location) {
    TtNode *node = arena_alloc_zero(arena, sizeof(TtNode));
    node->kind = kind;
    node->type = type != NULL ? type : &TYPE_UNIT_INSTANCE;
    node->location = location;
    return node;
}

// ── TT dump ───────────────────────────────────────────────────────────

/** Print @p indent levels of whitespace to stderr. */
static void dump_indent(int32_t indent) {
    for (int32_t i = 0; i < indent; i++) {
        fprintf(stderr, "  ");
    }
}

/** Return a short string label for a TtNodeKind. */
static const char *tt_node_kind_string(TtNodeKind kind) {
    switch (kind) {
    case TT_FILE:
        return "File";
    case TT_MODULE:
        return "Module";
    case TT_TYPE_ALIAS:
        return "TypeAlias";
    case TT_FUNCTION_DECLARATION:
        return "FunctionDecl";
    case TT_PARAMETER:
        return "Param";
    case TT_VARIABLE_DECLARATION:
        return "VarDecl";
    case TT_RETURN:
        return "Return";
    case TT_ASSIGN:
        return "Assign";
    case TT_BREAK:
        return "Break";
    case TT_CONTINUE:
        return "Continue";
    case TT_DEFER:
        return "Defer";
    case TT_BOOL_LITERAL:
        return "BoolLit";
    case TT_INT_LITERAL:
        return "IntLit";
    case TT_FLOAT_LITERAL:
        return "FloatLit";
    case TT_CHAR_LITERAL:
        return "CharLit";
    case TT_STRING_LITERAL:
        return "StringLit";
    case TT_UNIT_LITERAL:
        return "UnitLit";
    case TT_ARRAY_LITERAL:
        return "ArrayLit";
    case TT_TUPLE_LITERAL:
        return "TupleLit";
    case TT_VARIABLE_REFERENCE:
        return "VarRef";
    case TT_MODULE_ACCESS:
        return "ModuleAccess";
    case TT_INDEX:
        return "Index";
    case TT_TUPLE_INDEX:
        return "TupleIndex";
    case TT_UNARY:
        return "Unary";
    case TT_BINARY:
        return "Binary";
    case TT_CALL:
        return "Call";
    case TT_TYPE_CONVERSION:
        return "TypeConv";
    case TT_IF:
        return "If";
    case TT_BLOCK:
        return "Block";
    case TT_LOOP:
        return "Loop";
    case TT_STRUCT_DECLARATION:
        return "StructDecl";
    case TT_STRUCT_LITERAL:
        return "StructLit";
    case TT_STRUCT_FIELD_ACCESS:
        return "StructFieldAccess";
    case TT_METHOD_CALL:
        return "MethodCall";
    case TT_HEAP_ALLOC:
        return "HeapAlloc";
    case TT_ADDRESS_OF:
        return "AddressOf";
    case TT_DEREF:
        return "Deref";
    case TT_ENUM_DECLARATION:
        return "EnumDecl";
    case TT_MATCH:
        return "Match";
    }
    return "?";
}

static void tt_dump_children(TtNode **children, int32_t count, int32_t indent);

static void dump_literal(const TtNode *node) {
    switch (node->kind) {
    case TT_BOOL_LITERAL:
        fprintf(stderr, " %s\n", node->bool_literal.value ? "true" : "false");
        break;
    case TT_INT_LITERAL:
        fprintf(stderr, " %lu\n", (unsigned long)node->int_literal.value);
        break;
    case TT_FLOAT_LITERAL:
        fprintf(stderr, " %g\n", node->float_literal.value);
        break;
    case TT_CHAR_LITERAL:
        fprintf(stderr, " U+%04X\n", node->char_literal.value);
        break;
    case TT_STRING_LITERAL:
        fprintf(stderr, " \"%s\"\n", node->string_literal.value);
        break;
    default:
        fprintf(stderr, "\n");
        break;
    }
}

static void dump_unary_binary(const TtNode *node, int32_t indent) {
    if (node->kind == TT_UNARY) {
        fprintf(stderr, " %s\n", token_kind_string(node->unary.op));
        tt_dump(node->unary.operand, indent + 1);
    } else {
        fprintf(stderr, " %s\n", token_kind_string(node->binary.op));
        tt_dump(node->binary.left, indent + 1);
        tt_dump(node->binary.right, indent + 1);
    }
}

static void dump_function_decl(const TtNode *node, int32_t indent) {
    fprintf(stderr, " \"%s\"%s\n", node->function_declaration.name,
            node->function_declaration.is_public ? " pub" : "");
    tt_dump_children(node->function_declaration.params,
                     BUFFER_LENGTH(node->function_declaration.params), indent + 1);
    if (node->function_declaration.body != NULL) {
        tt_dump(node->function_declaration.body, indent + 1);
    }
}

static void dump_var_decl(const TtNode *node, int32_t indent) {
    fprintf(stderr, " \"%s\"%s\n", node->variable_declaration.name,
            node->variable_declaration.is_mut ? " mut" : "");
    if (node->variable_declaration.initializer != NULL) {
        tt_dump(node->variable_declaration.initializer, indent + 1);
    }
}

static void dump_block(const TtNode *node, int32_t indent) {
    fprintf(stderr, "\n");
    tt_dump_children(node->block.statements, BUFFER_LENGTH(node->block.statements), indent + 1);
    if (node->block.result != NULL) {
        dump_indent(indent + 1);
        fprintf(stderr, "result:\n");
        tt_dump(node->block.result, indent + 2);
    }
}

static void dump_match_node(const TtNode *node, int32_t indent) {
    fprintf(stderr, "\n");
    tt_dump(node->match_expr.operand, indent + 1);
    for (int32_t i = 0; i < BUFFER_LENGTH(node->match_expr.arm_bodies); i++) {
        dump_indent(indent + 1);
        fprintf(stderr, "arm %d:\n", i);
        if (node->match_expr.arm_conditions[i] != NULL) {
            tt_dump(node->match_expr.arm_conditions[i], indent + 2);
        }
        if (node->match_expr.arm_guards[i] != NULL) {
            dump_indent(indent + 2);
            fprintf(stderr, "guard:\n");
            tt_dump(node->match_expr.arm_guards[i], indent + 3);
        }
        tt_dump(node->match_expr.arm_bodies[i], indent + 2);
    }
}

static void dump_if_node(const TtNode *node, int32_t indent) {
    fprintf(stderr, "\n");
    tt_dump(node->if_expression.condition, indent + 1);
    tt_dump(node->if_expression.then_body, indent + 1);
    if (node->if_expression.else_body != NULL) {
        tt_dump(node->if_expression.else_body, indent + 1);
    }
}

void tt_dump(const TtNode *node, int32_t indent) {
    if (node == NULL) {
        dump_indent(indent);
        fprintf(stderr, "<null>\n");
        return;
    }

    dump_indent(indent);
    fprintf(stderr, "%s", tt_node_kind_string(node->kind));

    switch (node->kind) {
    case TT_FILE:
        fprintf(stderr, "\n");
        tt_dump_children(node->file.declarations, BUFFER_LENGTH(node->file.declarations),
                         indent + 1);
        break;

    case TT_MODULE:
        fprintf(stderr, " \"%s\"\n", node->module.name);
        break;

    case TT_TYPE_ALIAS:
        fprintf(stderr, " \"%s\"%s\n", node->type_alias.name,
                node->type_alias.is_public ? " pub" : "");
        break;

    case TT_FUNCTION_DECLARATION:
        dump_function_decl(node, indent);
        break;

    case TT_PARAMETER:
        fprintf(stderr, " \"%s\"\n", node->parameter.name);
        break;

    case TT_VARIABLE_DECLARATION:
        dump_var_decl(node, indent);
        break;

    case TT_RETURN:
        fprintf(stderr, "\n");
        if (node->return_statement.value != NULL) {
            tt_dump(node->return_statement.value, indent + 1);
        }
        break;

    case TT_ASSIGN:
        fprintf(stderr, "\n");
        tt_dump(node->assign.target, indent + 1);
        tt_dump(node->assign.value, indent + 1);
        break;

    case TT_BREAK:
        fprintf(stderr, "\n");
        if (node->break_statement.value != NULL) {
            tt_dump(node->break_statement.value, indent + 1);
        }
        break;

    case TT_CONTINUE:
        fprintf(stderr, "\n");
        break;

    case TT_DEFER:
        fprintf(stderr, "\n");
        tt_dump(node->defer_statement.body, indent + 1);
        break;

    case TT_BOOL_LITERAL:
    case TT_INT_LITERAL:
    case TT_FLOAT_LITERAL:
    case TT_CHAR_LITERAL:
    case TT_STRING_LITERAL:
    case TT_UNIT_LITERAL:
        dump_literal(node);
        break;

    case TT_ARRAY_LITERAL:
        fprintf(stderr, "\n");
        tt_dump_children(node->array_literal.elements, BUFFER_LENGTH(node->array_literal.elements),
                         indent + 1);
        break;

    case TT_TUPLE_LITERAL:
        fprintf(stderr, "\n");
        tt_dump_children(node->tuple_literal.elements, BUFFER_LENGTH(node->tuple_literal.elements),
                         indent + 1);
        break;

    case TT_VARIABLE_REFERENCE:
        fprintf(stderr, " \"%s\"\n", tt_symbol_name(node->variable_reference.symbol));
        break;

    case TT_MODULE_ACCESS:
        fprintf(stderr, " .%s\n", node->module_access.member);
        tt_dump(node->module_access.object, indent + 1);
        break;

    case TT_INDEX:
        fprintf(stderr, "\n");
        tt_dump(node->index_access.object, indent + 1);
        tt_dump(node->index_access.index, indent + 1);
        break;

    case TT_TUPLE_INDEX:
        fprintf(stderr, " .%d\n", node->tuple_index.element_index);
        tt_dump(node->tuple_index.object, indent + 1);
        break;

    case TT_UNARY:
    case TT_BINARY:
        dump_unary_binary(node, indent);
        break;

    case TT_CALL:
        fprintf(stderr, "\n");
        tt_dump(node->call.callee, indent + 1);
        tt_dump_children(node->call.arguments, BUFFER_LENGTH(node->call.arguments), indent + 1);
        break;

    case TT_TYPE_CONVERSION:
        fprintf(stderr, "\n");
        tt_dump(node->type_conversion.operand, indent + 1);
        break;

    case TT_IF:
        dump_if_node(node, indent);
        break;

    case TT_BLOCK:
        dump_block(node, indent);
        break;

    case TT_LOOP:
        fprintf(stderr, "\n");
        tt_dump(node->loop.body, indent + 1);
        break;

    case TT_STRUCT_DECLARATION:
        fprintf(stderr, " \"%s\"\n", node->struct_decl.name);
        break;

    case TT_STRUCT_LITERAL:
        fprintf(stderr, "\n");
        tt_dump_children(node->struct_literal.field_values,
                         BUFFER_LENGTH(node->struct_literal.field_values), indent + 1);
        break;

    case TT_STRUCT_FIELD_ACCESS:
        fprintf(stderr, " .%s%s\n", node->struct_field_access.field,
                node->struct_field_access.via_pointer ? " (ptr)" : "");
        tt_dump(node->struct_field_access.object, indent + 1);
        break;

    case TT_METHOD_CALL:
        fprintf(stderr, " %s\n", node->method_call.mangled_name);
        tt_dump(node->method_call.receiver, indent + 1);
        tt_dump_children(node->method_call.arguments, BUFFER_LENGTH(node->method_call.arguments),
                         indent + 1);
        break;

    case TT_HEAP_ALLOC:
        fprintf(stderr, "\n");
        tt_dump(node->heap_alloc.operand, indent + 1);
        break;

    case TT_ADDRESS_OF:
        fprintf(stderr, "\n");
        tt_dump(node->address_of.operand, indent + 1);
        break;

    case TT_DEREF:
        fprintf(stderr, "\n");
        tt_dump(node->deref.operand, indent + 1);
        break;

    case TT_ENUM_DECLARATION:
        fprintf(stderr, " \"%s\"\n", node->enum_decl.name);
        break;

    case TT_MATCH:
        dump_match_node(node, indent);
        break;
    }
}

static void tt_dump_children(TtNode **children, int32_t count, int32_t indent) {
    for (int32_t i = 0; i < count; i++) {
        tt_dump(children[i], indent);
    }
}

// ── TT child visitor ──────────────────────────────────────────────────

static void visit_buffer(TtChildVisitor visitor, void *ctx, TtNode **buf, int32_t count) {
    for (int32_t i = 0; i < count; i++) {
        visitor(ctx, &buf[i]);
    }
}

void tt_visit_children(TtNode *node, TtChildVisitor visitor, void *context) {
    if (node == NULL) {
        return;
    }
    switch (node->kind) {
    case TT_FILE:
        visit_buffer(visitor, context, node->file.declarations,
                     BUFFER_LENGTH(node->file.declarations));
        break;
    case TT_FUNCTION_DECLARATION:
        visit_buffer(visitor, context, node->function_declaration.params,
                     BUFFER_LENGTH(node->function_declaration.params));
        if (node->function_declaration.body != NULL) {
            visitor(context, &node->function_declaration.body);
        }
        break;
    case TT_BLOCK:
        visit_buffer(visitor, context, node->block.statements,
                     BUFFER_LENGTH(node->block.statements));
        if (node->block.result != NULL) {
            visitor(context, &node->block.result);
        }
        break;
    case TT_VARIABLE_DECLARATION:
        if (node->variable_declaration.initializer != NULL) {
            visitor(context, &node->variable_declaration.initializer);
        }
        break;
    case TT_RETURN:
        if (node->return_statement.value != NULL) {
            visitor(context, &node->return_statement.value);
        }
        break;
    case TT_ASSIGN:
        visitor(context, &node->assign.target);
        visitor(context, &node->assign.value);
        break;
    case TT_BINARY:
        visitor(context, &node->binary.left);
        visitor(context, &node->binary.right);
        break;
    case TT_UNARY:
        visitor(context, &node->unary.operand);
        break;
    case TT_CALL:
        visitor(context, &node->call.callee);
        visit_buffer(visitor, context, node->call.arguments, BUFFER_LENGTH(node->call.arguments));
        break;
    case TT_IF:
        visitor(context, &node->if_expression.condition);
        visitor(context, &node->if_expression.then_body);
        if (node->if_expression.else_body != NULL) {
            visitor(context, &node->if_expression.else_body);
        }
        break;
    case TT_ARRAY_LITERAL:
        visit_buffer(visitor, context, node->array_literal.elements,
                     BUFFER_LENGTH(node->array_literal.elements));
        break;
    case TT_TUPLE_LITERAL:
        visit_buffer(visitor, context, node->tuple_literal.elements,
                     BUFFER_LENGTH(node->tuple_literal.elements));
        break;
    case TT_INDEX:
        visitor(context, &node->index_access.object);
        visitor(context, &node->index_access.index);
        break;
    case TT_TUPLE_INDEX:
        visitor(context, &node->tuple_index.object);
        break;
    case TT_TYPE_CONVERSION:
        visitor(context, &node->type_conversion.operand);
        break;
    case TT_MODULE_ACCESS:
        visitor(context, &node->module_access.object);
        break;
    case TT_LOOP:
        visitor(context, &node->loop.body);
        break;
    case TT_DEFER:
        visitor(context, &node->defer_statement.body);
        break;
    case TT_STRUCT_DECLARATION:
        break;
    case TT_STRUCT_LITERAL:
        visit_buffer(visitor, context, node->struct_literal.field_values,
                     BUFFER_LENGTH(node->struct_literal.field_values));
        break;
    case TT_STRUCT_FIELD_ACCESS:
        visitor(context, &node->struct_field_access.object);
        break;
    case TT_METHOD_CALL:
        visitor(context, &node->method_call.receiver);
        visit_buffer(visitor, context, node->method_call.arguments,
                     BUFFER_LENGTH(node->method_call.arguments));
        break;
    case TT_HEAP_ALLOC:
        visitor(context, &node->heap_alloc.operand);
        break;
    case TT_ADDRESS_OF:
        visitor(context, &node->address_of.operand);
        break;
    case TT_DEREF:
        visitor(context, &node->deref.operand);
        break;
    case TT_ENUM_DECLARATION:
        break;
    case TT_MATCH:
        visitor(context, &node->match_expr.operand);
        visit_buffer(visitor, context, node->match_expr.arm_conditions,
                     BUFFER_LENGTH(node->match_expr.arm_conditions));
        visit_buffer(visitor, context, node->match_expr.arm_guards,
                     BUFFER_LENGTH(node->match_expr.arm_guards));
        visit_buffer(visitor, context, node->match_expr.arm_bodies,
                     BUFFER_LENGTH(node->match_expr.arm_bodies));
        break;
    default:
        break;
    }
}
