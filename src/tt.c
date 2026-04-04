#include "tt.h"

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
    TtSymbol *symbol = arena_alloc(arena, sizeof(TtSymbol));
    memset(symbol, 0, sizeof(TtSymbol));
    symbol->kind = kind;
    symbol->sema_symbol = sema_symbol;
    symbol->is_mut = is_mut;
    symbol->location = location;
    return symbol;
}

// ── TtNode constructors ───────────────────────────────────────────────

TtNode *tt_new(Arena *arena, TtNodeKind kind, const Type *type, SourceLocation location) {
    TtNode *node = arena_alloc(arena, sizeof(TtNode));
    memset(node, 0, sizeof(TtNode));
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
    case TT_FUNCTION_DECL:
        return "FunctionDecl";
    case TT_PARAM:
        return "Param";
    case TT_VAR_DECL:
        return "VarDecl";
    case TT_RETURN:
        return "Return";
    case TT_ASSIGN:
        return "Assign";
    case TT_BREAK:
        return "Break";
    case TT_CONTINUE:
        return "Continue";
    case TT_EXPR_STMT:
        return "ExprStmt";
    case TT_BOOL_LIT:
        return "BoolLit";
    case TT_INT_LIT:
        return "IntLit";
    case TT_FLOAT_LIT:
        return "FloatLit";
    case TT_CHAR_LIT:
        return "CharLit";
    case TT_STRING_LIT:
        return "StringLit";
    case TT_UNIT_LIT:
        return "UnitLit";
    case TT_ARRAY_LIT:
        return "ArrayLit";
    case TT_TUPLE_LIT:
        return "TupleLit";
    case TT_VAR_REF:
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
    case TT_TYPE_CONV:
        return "TypeConv";
    case TT_IF:
        return "If";
    case TT_BLOCK:
        return "Block";
    case TT_LOOP:
        return "Loop";
    }
    return "?";
}

/** Return the token-kind operator name for unary/binary display. */
static const char *op_string(TokenKind op) {
    switch (op) {
    case TOKEN_PLUS:
        return "+";
    case TOKEN_MINUS:
        return "-";
    case TOKEN_STAR:
        return "*";
    case TOKEN_SLASH:
        return "/";
    case TOKEN_PERCENT:
        return "%";
    case TOKEN_EQUAL_EQUAL:
        return "==";
    case TOKEN_BANG_EQUAL:
        return "!=";
    case TOKEN_LESS:
        return "<";
    case TOKEN_LESS_EQUAL:
        return "<=";
    case TOKEN_GREATER:
        return ">";
    case TOKEN_GREATER_EQUAL:
        return ">=";
    case TOKEN_AMPERSAND_AMPERSAND:
        return "&&";
    case TOKEN_PIPE_PIPE:
        return "||";
    case TOKEN_BANG:
        return "!";
    default:
        return "?op";
    }
}

static void tt_dump_children(TtNode **children, int32_t count, int32_t indent);

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

    case TT_FUNCTION_DECL:
        fprintf(stderr, " \"%s\"%s\n", node->function_decl.name,
                node->function_decl.is_public ? " pub" : "");
        tt_dump_children(node->function_decl.params, BUFFER_LENGTH(node->function_decl.params),
                         indent + 1);
        if (node->function_decl.body != NULL) {
            tt_dump(node->function_decl.body, indent + 1);
        }
        break;

    case TT_PARAM:
        fprintf(stderr, " \"%s\"\n", node->param.name);
        break;

    case TT_VAR_DECL:
        fprintf(stderr, " \"%s\"%s\n", node->var_decl.name, node->var_decl.is_mut ? " mut" : "");
        if (node->var_decl.initializer != NULL) {
            tt_dump(node->var_decl.initializer, indent + 1);
        }
        break;

    case TT_RETURN:
        fprintf(stderr, "\n");
        if (node->return_stmt.value != NULL) {
            tt_dump(node->return_stmt.value, indent + 1);
        }
        break;

    case TT_ASSIGN:
        fprintf(stderr, "\n");
        tt_dump(node->assign.target, indent + 1);
        tt_dump(node->assign.value, indent + 1);
        break;

    case TT_BREAK:
        fprintf(stderr, "\n");
        if (node->break_stmt.value != NULL) {
            tt_dump(node->break_stmt.value, indent + 1);
        }
        break;

    case TT_CONTINUE:
        fprintf(stderr, "\n");
        break;

    case TT_EXPR_STMT:
        fprintf(stderr, "\n");
        tt_dump(node->expr_stmt.expression, indent + 1);
        break;

    case TT_BOOL_LIT:
        fprintf(stderr, " %s\n", node->bool_lit.value ? "true" : "false");
        break;

    case TT_INT_LIT:
        fprintf(stderr, " %lu\n", (unsigned long)node->int_lit.value);
        break;

    case TT_FLOAT_LIT:
        fprintf(stderr, " %g\n", node->float_lit.value);
        break;

    case TT_CHAR_LIT:
        fprintf(stderr, " U+%04X\n", node->char_lit.value);
        break;

    case TT_STRING_LIT:
        fprintf(stderr, " \"%s\"\n", node->string_lit.value);
        break;

    case TT_UNIT_LIT:
        fprintf(stderr, "\n");
        break;

    case TT_ARRAY_LIT:
        fprintf(stderr, "\n");
        tt_dump_children(node->array_lit.elements, BUFFER_LENGTH(node->array_lit.elements),
                         indent + 1);
        break;

    case TT_TUPLE_LIT:
        fprintf(stderr, "\n");
        tt_dump_children(node->tuple_lit.elements, BUFFER_LENGTH(node->tuple_lit.elements),
                         indent + 1);
        break;

    case TT_VAR_REF:
        fprintf(stderr, " \"%s\"\n", tt_symbol_name(node->var_ref.symbol));
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
        fprintf(stderr, " %s\n", op_string(node->unary.op));
        tt_dump(node->unary.operand, indent + 1);
        break;

    case TT_BINARY:
        fprintf(stderr, " %s\n", op_string(node->binary.op));
        tt_dump(node->binary.left, indent + 1);
        tt_dump(node->binary.right, indent + 1);
        break;

    case TT_CALL:
        fprintf(stderr, "\n");
        tt_dump(node->call.callee, indent + 1);
        tt_dump_children(node->call.arguments, BUFFER_LENGTH(node->call.arguments), indent + 1);
        break;

    case TT_TYPE_CONV:
        fprintf(stderr, "\n");
        tt_dump(node->type_conv.operand, indent + 1);
        break;

    case TT_IF:
        fprintf(stderr, "\n");
        tt_dump(node->if_expr.condition, indent + 1);
        tt_dump(node->if_expr.then_body, indent + 1);
        if (node->if_expr.else_body != NULL) {
            tt_dump(node->if_expr.else_body, indent + 1);
        }
        break;

    case TT_BLOCK:
        fprintf(stderr, "\n");
        tt_dump_children(node->block.statements, BUFFER_LENGTH(node->block.statements), indent + 1);
        if (node->block.result != NULL) {
            dump_indent(indent + 1);
            fprintf(stderr, "result:\n");
            tt_dump(node->block.result, indent + 2);
        }
        break;

    case TT_LOOP:
        fprintf(stderr, "\n");
        tt_dump(node->loop.body, indent + 1);
        break;
    }
}

static void tt_dump_children(TtNode **children, int32_t count, int32_t indent) {
    for (int32_t i = 0; i < count; i++) {
        tt_dump(children[i], indent);
    }
}
