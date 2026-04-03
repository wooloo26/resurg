#include "code_generator.h"

/** Maps a Resurg variable name to its (possibly mangled) C identifier. */
struct VariableEntry {
    const char *rsg_name;
    const char *c_name; // may differ when shadowing is resolved
};

struct CodeGenerator {
    FILE *output;
    Arena *arena; // for temporary string building
    int32_t indent;
    const char *module;              // current module name (may be NULL)
    const char *source_file;         // escaped source path for rsg_assert
    int32_t temporary_counter;       // monotonic counter for _rsg_tmp_N
    int32_t string_builder_counter;  // monotonic counter for _rsg_sb_N
    VariableEntry *variables;        /* buf */
    int32_t shadow_variable_counter; // suffix counter for shadowed renames
};

// Output helpers - indented printing to the C file.

static void emit_indent(CodeGenerator *generator) {
    for (int32_t i = 0; i < generator->indent; i++) {
        fprintf(generator->output, "    ");
    }
}

static void emit(CodeGenerator *generator, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    vfprintf(generator->output, format, arguments);
    va_end(arguments);
}

static void emit_line(CodeGenerator *generator, const char *format, ...) {
    emit_indent(generator);
    va_list arguments;
    va_start(arguments, format);
    vfprintf(generator->output, format, arguments);
    va_end(arguments);
    fprintf(generator->output, "\n");
}

static const char *next_temporary(CodeGenerator *generator) {
    return arena_sprintf(generator->arena, "_rsg_tmp_%d", generator->temporary_counter++);
}

static const char *next_string_builder(CodeGenerator *generator) {
    return arena_sprintf(generator->arena, "_rsg_sb_%d", generator->string_builder_counter++);
}

// Variable name tracking - resolves Resurg names to C identifiers,
// appending a numeric suffix when a name shadows an outer binding.

static int32_t variable_find(const CodeGenerator *generator, const char *name) {
    for (int32_t i = BUFFER_LENGTH(generator->variables) - 1; i >= 0; i--) {
        if (strcmp(generator->variables[i].rsg_name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static const char *variable_lookup(const CodeGenerator *generator, const char *name) {
    int32_t index = variable_find(generator, name);
    return (index >= 0) ? generator->variables[index].c_name : name;
}

static const char *variable_define(CodeGenerator *generator, const char *name) {
    const char *c_name = (variable_find(generator, name) >= 0)
                             ? arena_sprintf(generator->arena, "%s__%d", name, generator->shadow_variable_counter++)
                             : name;
    VariableEntry entry = {name, c_name};
    BUFFER_PUSH(generator->variables, entry);
    return c_name;
}

static void variable_scope_reset(CodeGenerator *generator) {
    if (generator->variables != NULL) {
        BUFFER_FREE(generator->variables);
        generator->variables = NULL;
    }
    generator->shadow_variable_counter = 0;
}

/**
 * Prefix non-main function names with `rsg_` to avoid C reserved-word and
 * stdlib collisions.
 */
static const char *mangle_function_name(CodeGenerator *generator, const char *name) {
    if (strcmp(name, "main") == 0) {
        return "main";
    }
    return arena_sprintf(generator->arena, "rsg_%s", name);
}

// Forward declarations for mutually-recursive emitters.
static const char *emit_expression(CodeGenerator *generator, const ASTNode *node);
static void emit_statement(CodeGenerator *generator, const ASTNode *node);
static void emit_block_statements(CodeGenerator *generator, const ASTNode *block);

// Ternary optimisation helpers - detect if-expressions that can be emitted
// as C ternary operators instead of if/else statements with a temporary.

/**
 * Return the simple result expression from a branch body, or NULL if the
 * body contains statements that prevent ternary emission.
 */
static const ASTNode *simple_branch_result(const ASTNode *body) {
    if (body == NULL) {
        return NULL;
    }
    if (body->kind == NODE_BLOCK) {
        if (BUFFER_LENGTH(body->block.statements) != 0 || body->block.result == NULL) {
            return NULL;
        }
        return body->block.result;
    }
    // Direct expression (not a block)
    return body;
}

/**
 * Return true if the node evaluates to a C expression string without
 * emitting any side-effecting lines.
 */
static bool is_pure_expression(const ASTNode *node) {
    if (node == NULL) {
        return false;
    }
    switch (node->kind) {
    case NODE_LITERAL:
    case NODE_IDENTIFIER:
        return true;
    case NODE_UNARY:
        return is_pure_expression(node->unary.operand);
    case NODE_BINARY:
        return is_pure_expression(node->binary.left) && is_pure_expression(node->binary.right);
    default:
        return false;
    }
}

/**
 * Return true if the if-expression has both branches, and both produce
 * a trivial (pure) result - safe to emit as `cond ? a : b`.
 */
static bool is_simple_ternary(const ASTNode *node) {
    if (node->kind != NODE_IF || node->if_expression.else_body == NULL) {
        return false;
    }
    const ASTNode *then_result = simple_branch_result(node->if_expression.then_body);
    if (then_result == NULL || !is_pure_expression(then_result)) {
        return false;
    }
    const ASTNode *else_body = node->if_expression.else_body;
    if (else_body->kind == NODE_IF) {
        return false; // else-if chains stay as statements
    }
    const ASTNode *else_result = simple_branch_result(else_body);
    return else_result != NULL && is_pure_expression(else_result);
}

/** Map Resurg binary operator TokenKind to its C operator string. */
static const char *c_binary_operator(TokenKind op) {
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
    default:
        return "??";
    }
}

static const char *c_compound_operator(TokenKind op) {
    switch (op) {
    case TOKEN_PLUS_EQUAL:
        return "+=";
    case TOKEN_MINUS_EQUAL:
        return "-=";
    case TOKEN_STAR_EQUAL:
        return "*=";
    case TOKEN_SLASH_EQUAL:
        return "/=";
    default:
        return "?=";
    }
}

/** Escape @p source for embedding inside a C string literal. */
static const char *c_string_escape(const CodeGenerator *generator, const char *source) {
    if (source == NULL) {
        return "";
    }
    // Count extra space needed for escapes
    int32_t extra = 0;
    for (const char *pointer = source; *pointer != '\0'; pointer++) {
        switch (*pointer) {
        case '\\':
        case '"':
        case '\n':
        case '\r':
        case '\t':
            extra++;
            break;
        default:
            break;
        }
    }
    if (extra == 0) {
        return source;
    }
    int32_t length = (int32_t)strlen(source);
    char *buffer = arena_alloc(generator->arena, length + extra + 1);
    int32_t write_index = 0;
    for (const char *pointer = source; *pointer != '\0'; pointer++) {
        switch (*pointer) {
        case '\\':
            buffer[write_index++] = '\\';
            buffer[write_index++] = '\\';
            break;
        case '"':
            buffer[write_index++] = '\\';
            buffer[write_index++] = '"';
            break;
        case '\n':
            buffer[write_index++] = '\\';
            buffer[write_index++] = 'n';
            break;
        case '\r':
            buffer[write_index++] = '\\';
            buffer[write_index++] = 'r';
            break;
        case '\t':
            buffer[write_index++] = '\\';
            buffer[write_index++] = 't';
            break;
        default:
            buffer[write_index++] = *pointer;
            break;
        }
    }
    buffer[write_index] = '\0';
    return buffer;
}

/** Escape a file path for embedding in C (backslash -> forward slash). */
static const char *c_escape_file_path(const CodeGenerator *generator, const char *path) {
    if (path == NULL) {
        return "";
    }
    int32_t length = (int32_t)strlen(path);
    char *buffer = arena_alloc(generator->arena, length + 1);
    for (int32_t i = 0; i < length; i++) {
        buffer[i] = (char)((path[i] == '\\') ? '/' : path[i]);
    }
    buffer[length] = '\0';
    return buffer;
}

/**
 * Format @p value as a C double literal, ensuring a trailing .0 when no
 * decimal point or exponent is present.
 */
static const char *format_float64(const CodeGenerator *generator, double value) {
    char buffer[64];
    int32_t length = snprintf(buffer, sizeof(buffer), "%.17g", value);
    bool has_dot = false;
    for (int32_t i = 0; i < length; i++) {
        if (buffer[i] == '.' || buffer[i] == 'e' || buffer[i] == 'E') {
            has_dot = true;
            break;
        }
    }
    if (!has_dot) {
        buffer[length] = '.';
        buffer[length + 1] = '0';
        buffer[length + 2] = '\0';
    }
    return arena_strdup(generator->arena, buffer);
}

/** Format @p value as a C float literal with the f suffix. */
static const char *format_float32(const CodeGenerator *generator, double value) {
    char buffer[64];
    int32_t length = snprintf(buffer, sizeof(buffer), "%.9g", value);
    bool has_dot = false;
    for (int32_t i = 0; i < length; i++) {
        if (buffer[i] == '.' || buffer[i] == 'e' || buffer[i] == 'E') {
            has_dot = true;
            break;
        }
    }
    if (!has_dot) {
        buffer[length] = '.';
        buffer[length + 1] = '0';
        buffer[length + 2] = '\0';
        length += 2;
    }
    buffer[length] = 'f';
    buffer[length + 1] = '\0';
    return arena_strdup(generator->arena, buffer);
}

// Type naming helpers for compound types (arrays, tuples).

/** Generate a clean C identifier suffix for @p type. */
static const char *type_tag(CodeGenerator *gen, const Type *type) {
    switch (type->kind) {
    case TYPE_BOOL:
        return "bool";
    case TYPE_I8:
        return "i8";
    case TYPE_I16:
        return "i16";
    case TYPE_I32:
        return "i32";
    case TYPE_I64:
        return "i64";
    case TYPE_I128:
        return "i128";
    case TYPE_U8:
        return "u8";
    case TYPE_U16:
        return "u16";
    case TYPE_U32:
        return "u32";
    case TYPE_U64:
        return "u64";
    case TYPE_U128:
        return "u128";
    case TYPE_ISIZE:
        return "isize";
    case TYPE_USIZE:
        return "usize";
    case TYPE_F32:
        return "f32";
    case TYPE_F64:
        return "f64";
    case TYPE_CHAR:
        return "char";
    case TYPE_STRING:
        return "str";
    case TYPE_UNIT:
        return "unit";
    case TYPE_ARRAY:
        return arena_sprintf(gen->arena, "Arr_%s_%d", type_tag(gen, type->array_element), type->array_size);
    case TYPE_TUPLE: {
        const char *result = "Tup";
        for (int32_t i = 0; i < type->tuple_count; i++) {
            result = arena_sprintf(gen->arena, "%s_%s", result, type_tag(gen, type->tuple_elements[i]));
        }
        return result;
    }
    default:
        return "err";
    }
}

/** Return the C type name for any type, generating struct names for compounds. */
static const char *c_type_for(CodeGenerator *gen, const Type *type) {
    if (type == NULL) {
        return "/* ? */";
    }
    if (type->kind == TYPE_ARRAY || type->kind == TYPE_TUPLE) {
        return arena_sprintf(gen->arena, "_Rsg%s", type_tag(gen, type));
    }
    return c_type_string(type);
}

// Compound type collection - walk AST to discover all array/tuple types.

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
            BUFFER_PUSH(g_compound_types, type);
        }
    } else if (type->kind == TYPE_TUPLE) {
        for (int32_t i = 0; i < type->tuple_count; i++) {
            register_compound_type(type->tuple_elements[i]);
        }
        if (!compound_type_registered(type)) {
            BUFFER_PUSH(g_compound_types, type);
        }
    }
}

/** Recursively walk @p node collecting all array/tuple types. */
static void collect_compound_types(const ASTNode *node) {
    if (node == NULL) {
        return;
    }
    if (node->type != NULL) {
        register_compound_type(node->type);
    }
    switch (node->kind) {
    case NODE_FILE:
        for (int32_t i = 0; i < BUFFER_LENGTH(node->file.declarations); i++) {
            collect_compound_types(node->file.declarations[i]);
        }
        break;
    case NODE_FUNCTION_DECLARATION:
        for (int32_t i = 0; i < BUFFER_LENGTH(node->function_declaration.parameters); i++) {
            collect_compound_types(node->function_declaration.parameters[i]);
        }
        collect_compound_types(node->function_declaration.body);
        break;
    case NODE_BLOCK:
        for (int32_t i = 0; i < BUFFER_LENGTH(node->block.statements); i++) {
            collect_compound_types(node->block.statements[i]);
        }
        collect_compound_types(node->block.result);
        break;
    case NODE_VARIABLE_DECLARATION:
        collect_compound_types(node->variable_declaration.initializer);
        break;
    case NODE_EXPRESSION_STATEMENT:
        collect_compound_types(node->expression_statement.expression);
        break;
    case NODE_BINARY:
        collect_compound_types(node->binary.left);
        collect_compound_types(node->binary.right);
        break;
    case NODE_UNARY:
        collect_compound_types(node->unary.operand);
        break;
    case NODE_CALL:
        collect_compound_types(node->call.callee);
        for (int32_t i = 0; i < BUFFER_LENGTH(node->call.arguments); i++) {
            collect_compound_types(node->call.arguments[i]);
        }
        break;
    case NODE_IF:
        collect_compound_types(node->if_expression.condition);
        collect_compound_types(node->if_expression.then_body);
        collect_compound_types(node->if_expression.else_body);
        break;
    case NODE_ASSIGN:
        collect_compound_types(node->assign.target);
        collect_compound_types(node->assign.value);
        break;
    case NODE_COMPOUND_ASSIGN:
        collect_compound_types(node->compound_assign.target);
        collect_compound_types(node->compound_assign.value);
        break;
    case NODE_ARRAY_LITERAL:
        for (int32_t i = 0; i < BUFFER_LENGTH(node->array_literal.elements); i++) {
            collect_compound_types(node->array_literal.elements[i]);
        }
        break;
    case NODE_TUPLE_LITERAL:
        for (int32_t i = 0; i < BUFFER_LENGTH(node->tuple_literal.elements); i++) {
            collect_compound_types(node->tuple_literal.elements[i]);
        }
        break;
    case NODE_INDEX:
        collect_compound_types(node->index_access.object);
        collect_compound_types(node->index_access.index);
        break;
    case NODE_TYPE_CONVERSION:
        collect_compound_types(node->type_conversion.operand);
        break;
    case NODE_MEMBER:
        collect_compound_types(node->member.object);
        break;
    case NODE_STRING_INTERPOLATION:
        for (int32_t i = 0; i < BUFFER_LENGTH(node->string_interpolation.parts); i++) {
            collect_compound_types(node->string_interpolation.parts[i]);
        }
        break;
    case NODE_LOOP:
        collect_compound_types(node->loop.body);
        break;
    case NODE_FOR:
        collect_compound_types(node->for_loop.start);
        collect_compound_types(node->for_loop.end);
        collect_compound_types(node->for_loop.body);
        break;
    default:
        break;
    }
}

/** Emit typedef structs for all collected compound types. */
static void emit_compound_typedefs(CodeGenerator *generator) {
    for (int32_t i = 0; i < BUFFER_LENGTH(g_compound_types); i++) {
        const Type *type = g_compound_types[i];
        const char *name = c_type_for(generator, type);
        if (type->kind == TYPE_ARRAY) {
            emit_line(generator, "typedef struct { %s _data[%d]; } %s;", c_type_for(generator, type->array_element),
                      type->array_size, name);
        } else if (type->kind == TYPE_TUPLE) {
            emit_indent(generator);
            fprintf(generator->output, "typedef struct {");
            for (int32_t j = 0; j < type->tuple_count; j++) {
                fprintf(generator->output, " %s _%d;", c_type_for(generator, type->tuple_elements[j]), j);
            }
            fprintf(generator->output, " } %s;\n", name);
        }
    }
    if (BUFFER_LENGTH(g_compound_types) > 0) {
        emit(generator, "\n");
    }
}

// Expression emitters - each returns a C expression string.  Some emit
// auxiliary lines (e.g. temporaries for if-expressions) as a side effect.

/** Emit an if/else branch body, optionally assigning the result to @p target. */
static void emit_branch(CodeGenerator *generator, const ASTNode *body, const char *target) {
    if (body->kind == NODE_BLOCK) {
        if (target != NULL) {
            for (int32_t i = 0; i < BUFFER_LENGTH(body->block.statements); i++) {
                emit_statement(generator, body->block.statements[i]);
            }
            if (body->block.result != NULL) {
                const char *value = emit_expression(generator, body->block.result);
                emit_line(generator, "%s = %s;", target, value);
            }
        } else {
            emit_block_statements(generator, body);
            if (body->block.result != NULL) {
                emit_statement(generator, body->block.result);
            }
        }
    } else {
        if (target != NULL) {
            const char *value = emit_expression(generator, body);
            emit_line(generator, "%s = %s;", target, value);
        } else {
            emit_statement(generator, body);
        }
    }
}

/**
 * Emit an if-expression or if-statement.  When @p target is non-NULL the
 * result of each branch is assigned to it; otherwise emitted as a pure
 * statement.
 */
static void emit_if(CodeGenerator *generator, const ASTNode *node, const char *target, bool is_else_if) {
    const char *condition_value = emit_expression(generator, node->if_expression.condition);
    if (is_else_if) {
        if (condition_value[0] == '(') {
            fprintf(generator->output, "if %s {\n", condition_value);
        } else {
            fprintf(generator->output, "if (%s) {\n", condition_value);
        }
    } else {
        if (condition_value[0] == '(') {
            emit_line(generator, "if %s {", condition_value);
        } else {
            emit_line(generator, "if (%s) {", condition_value);
        }
    }
    generator->indent++;

    emit_branch(generator, node->if_expression.then_body, target);
    generator->indent--;

    if (node->if_expression.else_body != NULL) {
        if (node->if_expression.else_body->kind == NODE_IF) {
            emit_indent(generator);
            fprintf(generator->output, "} else ");
            emit_if(generator, node->if_expression.else_body, target, true);
        } else {
            emit_line(generator, "} else {");
            generator->indent++;
            emit_branch(generator, node->if_expression.else_body, target);
            generator->indent--;
            emit_line(generator, "}");
        }
    } else {
        emit_line(generator, "}");
    }
}

/**
 * Convert an expression node to an RsgString value for interpolation,
 * wrapping non-string types with the appropriate rsg_string_from_* call.
 */
static const char *string_convert_expression(CodeGenerator *generator, const ASTNode *node) {
    const char *value = emit_expression(generator, node);
    const Type *type = node->type;
    if (type == NULL) {
        return value;
    }
    switch (type->kind) {
    case TYPE_STRING:
        return value;
    case TYPE_BOOL:
        return arena_sprintf(generator->arena, "rsg_string_from_bool(%s)", value);
    case TYPE_I8:
    case TYPE_I16:
    case TYPE_I32:
        return arena_sprintf(generator->arena, "rsg_string_from_i32((int32_t)(%s))", value);
    case TYPE_I64:
    case TYPE_I128:
    case TYPE_ISIZE:
        return arena_sprintf(generator->arena, "rsg_string_from_i64((int64_t)(%s))", value);
    case TYPE_U8:
    case TYPE_U16:
    case TYPE_U32:
        return arena_sprintf(generator->arena, "rsg_string_from_u32((uint32_t)(%s))", value);
    case TYPE_U64:
    case TYPE_U128:
    case TYPE_USIZE:
        return arena_sprintf(generator->arena, "rsg_string_from_u64((uint64_t)(%s))", value);
    case TYPE_F32:
        return arena_sprintf(generator->arena, "rsg_string_from_f32(%s)", value);
    case TYPE_F64:
        return arena_sprintf(generator->arena, "rsg_string_from_f64(%s)", value);
    case TYPE_CHAR:
        return arena_sprintf(generator->arena, "rsg_string_from_char(%s)", value);
    default:
        return arena_sprintf(generator->arena, "rsg_string_from_i32((int32_t)(%s))", value);
    }
}

// Per-node expression emitters.

static const char *emit_literal_expression(CodeGenerator *generator, const ASTNode *node) {
    switch (node->literal.kind) {
    case LITERAL_BOOL:
        return node->literal.boolean_value ? "true" : "false";
    case LITERAL_I8:
    case LITERAL_I16:
    case LITERAL_I32:
        return arena_sprintf(generator->arena, "%lld", (long long)(int64_t)node->literal.integer_value);
    case LITERAL_I64:
    case LITERAL_I128:
    case LITERAL_ISIZE:
        return arena_sprintf(generator->arena, "%lldLL", (long long)(int64_t)node->literal.integer_value);
    case LITERAL_U8:
    case LITERAL_U16:
    case LITERAL_U32:
        return arena_sprintf(generator->arena, "%lluU", (unsigned long long)node->literal.integer_value);
    case LITERAL_U64:
    case LITERAL_U128:
    case LITERAL_USIZE:
        return arena_sprintf(generator->arena, "%lluULL", (unsigned long long)node->literal.integer_value);
    case LITERAL_F32:
        return format_float32(generator, node->literal.float64_value);
    case LITERAL_F64:
        return format_float64(generator, node->literal.float64_value);
    case LITERAL_CHAR: {
        char c = node->literal.char_value;
        switch (c) {
        case '\n':
            return "'\\n'";
        case '\t':
            return "'\\t'";
        case '\\':
            return "'\\\\'";
        case '\'':
            return "'\\\''";
        case '\0':
            return "'\\0'";
        default:
            return arena_sprintf(generator->arena, "'%c'", c);
        }
    }
    case LITERAL_STRING: {
        const char *escaped = c_string_escape(generator, node->literal.string_value);
        return arena_sprintf(generator->arena, "rsg_string_literal(\"%s\")", escaped);
    }
    case LITERAL_UNIT:
        return "(void)0";
    }
    return "0";
}

static const char *emit_unary_expression(CodeGenerator *generator, const ASTNode *node) {
    // Fold negation into integer/float literals to avoid overflow issues
    if (node->unary.op == TOKEN_MINUS && node->unary.operand->kind == NODE_LITERAL) {
        const ASTNode *literal = node->unary.operand;
        switch (literal->literal.kind) {
        case LITERAL_I8:
        case LITERAL_I16:
            return arena_sprintf(generator->arena, "%lld", -(long long)(int64_t)literal->literal.integer_value);
        case LITERAL_I32: {
            int64_t negated = -(int64_t)literal->literal.integer_value;
            if (negated == (int64_t)(-2147483647 - 1)) {
                return "(-2147483647 - 1)";
            }
            return arena_sprintf(generator->arena, "%lld", (long long)negated);
        }
        case LITERAL_I64:
        case LITERAL_I128:
        case LITERAL_ISIZE: {
            uint64_t value = literal->literal.integer_value;
            if (value == (uint64_t)9223372036854775808ULL) {
                return "(-9223372036854775807LL - 1)";
            }
            return arena_sprintf(generator->arena, "%lldLL", -(long long)(int64_t)value);
        }
        case LITERAL_F32:
            return format_float32(generator, -literal->literal.float64_value);
        case LITERAL_F64:
            return format_float64(generator, -literal->literal.float64_value);
        default:
            break;
        }
    }
    const char *operand = emit_expression(generator, node->unary.operand);
    if (node->unary.op == TOKEN_BANG) {
        return arena_sprintf(generator->arena, "(!%s)", operand);
    }
    if (node->unary.op == TOKEN_MINUS) {
        return arena_sprintf(generator->arena, "(-%s)", operand);
    }
    return operand;
}

/** Attempt to constant-fold a binary op on two i32 literals. Returns NULL if not foldable. */
static const char *fold_i32_binary(CodeGenerator *generator, const ASTNode *node) {
    const ASTNode *left_operand = node->binary.left;
    const ASTNode *right_operand = node->binary.right;

    bool both_i32 = left_operand->kind == NODE_LITERAL && right_operand->kind == NODE_LITERAL &&
                    left_operand->literal.kind == LITERAL_I32 && right_operand->literal.kind == LITERAL_I32;
    if (!both_i32) {
        return NULL;
    }

    int64_t left_value = (int64_t)left_operand->literal.integer_value;
    int64_t right_value = (int64_t)right_operand->literal.integer_value;
    int64_t result;
    bool folded = true;
    switch (node->binary.op) {
    case TOKEN_PLUS:
        result = left_value + right_value;
        break;
    case TOKEN_MINUS:
        result = left_value - right_value;
        break;
    case TOKEN_STAR:
        result = left_value * right_value;
        break;
    case TOKEN_SLASH:
        folded = (right_value != 0);
        if (folded) {
            result = left_value / right_value;
        }
        break;
    case TOKEN_PERCENT:
        folded = (right_value != 0);
        if (folded) {
            result = left_value % right_value;
        }
        break;
    default:
        folded = false;
        break;
    }
    if (!folded) {
        return NULL;
    }
    if (result == (int64_t)(-2147483647 - 1)) {
        return "(-2147483647 - 1)";
    }
    return arena_sprintf(generator->arena, "%lld", (long long)result);
}

static const char *emit_binary_expression(CodeGenerator *generator, const ASTNode *node) {
    const char *folded = fold_i32_binary(generator, node);
    if (folded != NULL) {
        return folded;
    }

    const ASTNode *left_operand = node->binary.left;
    const ASTNode *right_operand = node->binary.right;
    const char *left = emit_expression(generator, left_operand);
    const char *right = emit_expression(generator, right_operand);

    const Type *left_type = left_operand->type;

    // String equality/inequality
    if (left_type != NULL && left_type->kind == TYPE_STRING) {
        if (node->binary.op == TOKEN_EQUAL_EQUAL) {
            return arena_sprintf(generator->arena, "rsg_string_equal(%s, %s)", left, right);
        }
        if (node->binary.op == TOKEN_BANG_EQUAL) {
            return arena_sprintf(generator->arena, "(!rsg_string_equal(%s, %s))", left, right);
        }
    }

    // Array equality/inequality - memcmp on ._data
    if (left_type != NULL && left_type->kind == TYPE_ARRAY &&
        (node->binary.op == TOKEN_EQUAL_EQUAL || node->binary.op == TOKEN_BANG_EQUAL)) {
        const char *left_tmp = next_temporary(generator);
        const char *right_tmp = next_temporary(generator);
        const char *tname = c_type_for(generator, left_type);
        emit_line(generator, "%s %s = %s;", tname, left_tmp, left);
        emit_line(generator, "%s %s = %s;", tname, right_tmp, right);
        const char *cmp = (node->binary.op == TOKEN_EQUAL_EQUAL) ? "==" : "!=";
        return arena_sprintf(generator->arena, "(memcmp(%s._data, %s._data, sizeof(%s._data)) %s 0)", left_tmp,
                             right_tmp, left_tmp, cmp);
    }

    // Tuple equality/inequality - element-wise comparison
    if (left_type != NULL && left_type->kind == TYPE_TUPLE &&
        (node->binary.op == TOKEN_EQUAL_EQUAL || node->binary.op == TOKEN_BANG_EQUAL)) {
        bool is_equal = (node->binary.op == TOKEN_EQUAL_EQUAL);
        const char *left_tmp = next_temporary(generator);
        const char *right_tmp = next_temporary(generator);
        const char *tname = c_type_for(generator, left_type);
        emit_line(generator, "%s %s = %s;", tname, left_tmp, left);
        emit_line(generator, "%s %s = %s;", tname, right_tmp, right);
        const char *join = is_equal ? " && " : " || ";
        const char *cmp = is_equal ? "==" : "!=";
        const char *result = "";
        for (int32_t i = 0; i < left_type->tuple_count; i++) {
            const char *l = arena_sprintf(generator->arena, "%s._%d", left_tmp, i);
            const char *r = arena_sprintf(generator->arena, "%s._%d", right_tmp, i);
            const char *part;
            if (left_type->tuple_elements[i]->kind == TYPE_STRING) {
                if (is_equal) {
                    part = arena_sprintf(generator->arena, "rsg_string_equal(%s, %s)", l, r);
                } else {
                    part = arena_sprintf(generator->arena, "(!rsg_string_equal(%s, %s))", l, r);
                }
            } else {
                part = arena_sprintf(generator->arena, "(%s %s %s)", l, cmp, r);
            }
            if (i == 0) {
                result = part;
            } else {
                result = arena_sprintf(generator->arena, "%s%s%s", result, join, part);
            }
        }
        return arena_sprintf(generator->arena, "(%s)", result);
    }

    const char *op = c_binary_operator(node->binary.op);
    return arena_sprintf(generator->arena, "(%s %s %s)", left, op, right);
}

static const char *emit_assert_call(CodeGenerator *generator, const ASTNode *node) {
    int32_t argument_count = BUFFER_LENGTH(node->call.arguments);
    const char *condition = argument_count > 0 ? emit_expression(generator, node->call.arguments[0]) : "0";
    const char *message = "NULL";
    if (argument_count > 1) {
        ASTNode *message_node = node->call.arguments[1];
        if (message_node->kind == NODE_LITERAL && message_node->literal.kind == LITERAL_STRING) {
            message = arena_sprintf(generator->arena, "\"%s\"",
                                    c_string_escape(generator, message_node->literal.string_value));
        } else {
            const char *message_expression = emit_expression(generator, message_node);
            message = arena_sprintf(generator->arena, "%s.data", message_expression);
        }
    }
    return arena_sprintf(generator->arena, "rsg_assert(%s, %s, _rsg_file, %d)", condition, message,
                         node->location.line);
}

static const char *emit_call_expression(CodeGenerator *generator, const ASTNode *node) {
    if (node->call.callee->kind == NODE_IDENTIFIER && strcmp(node->call.callee->identifier.name, "assert") == 0) {
        return emit_assert_call(generator, node);
    }
    const char *callee;
    if (node->call.callee->kind == NODE_IDENTIFIER) {
        callee = mangle_function_name(generator, node->call.callee->identifier.name);
    } else {
        callee = emit_expression(generator, node->call.callee);
    }
    const char *argument_list = "";
    for (int32_t i = 0; i < BUFFER_LENGTH(node->call.arguments); i++) {
        const char *argument = emit_expression(generator, node->call.arguments[i]);
        if (i == 0) {
            argument_list = argument;
        } else {
            argument_list = arena_sprintf(generator->arena, "%s, %s", argument_list, argument);
        }
    }
    return arena_sprintf(generator->arena, "%s(%s)", callee, argument_list);
}

static const char *emit_if_expression(CodeGenerator *generator, const ASTNode *node) {
    const Type *type = node->type;
    if (type == NULL || type->kind == TYPE_UNIT) {
        emit_if(generator, node, NULL, false);
        return "(void)0";
    }
    if (is_simple_ternary(node)) {
        const char *condition = emit_expression(generator, node->if_expression.condition);
        const ASTNode *then_result = simple_branch_result(node->if_expression.then_body);
        const ASTNode *else_result = simple_branch_result(node->if_expression.else_body);
        const char *then_value = emit_expression(generator, then_result);
        const char *else_value = emit_expression(generator, else_result);
        return arena_sprintf(generator->arena, "(%s ? %s : %s)", condition, then_value, else_value);
    }
    const char *temporary = next_temporary(generator);
    emit_line(generator, "%s %s;", c_type_for(generator, type), temporary);
    emit_if(generator, node, temporary, false);
    return temporary;
}

static const char *emit_block_expression(CodeGenerator *generator, const ASTNode *node) {
    for (int32_t i = 0; i < BUFFER_LENGTH(node->block.statements); i++) {
        emit_statement(generator, node->block.statements[i]);
    }
    if (node->block.result != NULL) {
        return emit_expression(generator, node->block.result);
    }
    return "(void)0";
}

static const char *emit_string_interpolation_expression(CodeGenerator *generator, const ASTNode *node) {
    int32_t part_count = BUFFER_LENGTH(node->string_interpolation.parts);
    const char **string_parts = NULL;
    for (int32_t i = 0; i < part_count; i++) {
        const ASTNode *part = node->string_interpolation.parts[i];
        if (part->kind == NODE_LITERAL && part->literal.kind == LITERAL_STRING) {
            const char *text = part->literal.string_value;
            if (text == NULL || text[0] == '\0') {
                continue;
            }
            BUFFER_PUSH(string_parts, arena_sprintf(generator->arena, "rsg_string_literal(\"%s\")",
                                                    c_string_escape(generator, text)));
        } else {
            BUFFER_PUSH(string_parts, string_convert_expression(generator, part));
        }
    }
    int32_t count = BUFFER_LENGTH(string_parts);

    if (count == 1) {
        const char *result = string_parts[0];
        BUFFER_FREE(string_parts);
        return result;
    }
    if (count == 2) {
        const char *result =
            arena_sprintf(generator->arena, "rsg_string_concat(%s, %s)", string_parts[0], string_parts[1]);
        BUFFER_FREE(string_parts);
        return result;
    }

    const char *builder = next_string_builder(generator);
    emit_line(generator, "RsgStringBuilder %s;", builder);
    emit_line(generator, "rsg_string_builder_init(&%s);", builder);
    for (int32_t i = 0; i < count; i++) {
        emit_line(generator, "rsg_string_builder_append_string(&%s, %s);", builder, string_parts[i]);
    }
    BUFFER_FREE(string_parts);
    const char *temporary = next_temporary(generator);
    emit_line(generator, "RsgString %s = rsg_string_builder_finish(&%s);", temporary, builder);
    return temporary;
}

static const char *emit_array_literal_expression(CodeGenerator *generator, const ASTNode *node) {
    const Type *type = node->type;
    const char *tname = c_type_for(generator, type);
    const char *result = arena_sprintf(generator->arena, "(%s){ ._data = { ", tname);
    for (int32_t i = 0; i < BUFFER_LENGTH(node->array_literal.elements); i++) {
        if (i > 0) {
            result = arena_sprintf(generator->arena, "%s, ", result);
        }
        const char *elem = emit_expression(generator, node->array_literal.elements[i]);
        result = arena_sprintf(generator->arena, "%s%s", result, elem);
    }
    return arena_sprintf(generator->arena, "%s } }", result);
}

static const char *emit_tuple_literal_expression(CodeGenerator *generator, const ASTNode *node) {
    const Type *type = node->type;
    const char *tname = c_type_for(generator, type);
    const char *result = arena_sprintf(generator->arena, "(%s){ ", tname);
    for (int32_t i = 0; i < BUFFER_LENGTH(node->tuple_literal.elements); i++) {
        if (i > 0) {
            result = arena_sprintf(generator->arena, "%s, ", result);
        }
        const char *elem = emit_expression(generator, node->tuple_literal.elements[i]);
        result = arena_sprintf(generator->arena, "%s._%d = %s", result, i, elem);
    }
    return arena_sprintf(generator->arena, "%s }", result);
}

static const char *emit_index_expression(CodeGenerator *generator, const ASTNode *node) {
    const char *object = emit_expression(generator, node->index_access.object);
    const char *index = emit_expression(generator, node->index_access.index);
    return arena_sprintf(generator->arena, "%s._data[%s]", object, index);
}

static const char *emit_type_conversion_expression(CodeGenerator *generator, const ASTNode *node) {
    return emit_expression(generator, node->type_conversion.operand);
}

static const char *emit_member_expression(CodeGenerator *generator, const ASTNode *node) {
    const char *object = emit_expression(generator, node->member.object);
    return arena_sprintf(generator->arena, "%s._%s", object, node->member.member);
}

/** Expression dispatch - returns a C expression string for @p node. */
static const char *emit_expression(CodeGenerator *generator, const ASTNode *node) {
    if (node == NULL) {
        return "0";
    }
    switch (node->kind) {
    case NODE_LITERAL:
        return emit_literal_expression(generator, node);
    case NODE_IDENTIFIER:
        return variable_lookup(generator, node->identifier.name);
    case NODE_UNARY:
        return emit_unary_expression(generator, node);
    case NODE_BINARY:
        return emit_binary_expression(generator, node);
    case NODE_CALL:
        return emit_call_expression(generator, node);
    case NODE_MEMBER:
        return emit_member_expression(generator, node);
    case NODE_INDEX:
        return emit_index_expression(generator, node);
    case NODE_IF:
        return emit_if_expression(generator, node);
    case NODE_BLOCK:
        return emit_block_expression(generator, node);
    case NODE_STRING_INTERPOLATION:
        return emit_string_interpolation_expression(generator, node);
    case NODE_ARRAY_LITERAL:
        return emit_array_literal_expression(generator, node);
    case NODE_TUPLE_LITERAL:
        return emit_tuple_literal_expression(generator, node);
    case NODE_TYPE_CONVERSION:
        return emit_type_conversion_expression(generator, node);
    default:
        return "0";
    }
}

/**
 * Emit the statements of a block (excludes the trailing result expression
 * - the caller is responsible for that).
 */
static void emit_block_statements(CodeGenerator *generator, const ASTNode *block) {
    if (block == NULL || block->kind != NODE_BLOCK) {
        return;
    }
    for (int32_t i = 0; i < BUFFER_LENGTH(block->block.statements); i++) {
        emit_statement(generator, block->block.statements[i]);
    }
    // Note: block.result is NOT emitted here (caller handles it)
}

// Per-node statement emitters.

static void emit_variable_declaration_statement(CodeGenerator *generator, const ASTNode *node) {
    const Type *type = node->type;
    if (type == NULL) {
        type = node->variable_declaration.initializer != NULL ? node->variable_declaration.initializer->type : NULL;
    }
    if (type == NULL && node->variable_declaration.type.kind == AST_TYPE_NAME) {
        type = type_from_name(node->variable_declaration.type.name);
    }
    if (type == NULL) {
        type = &TYPE_I32_INSTANCE;
    }
    const char *c_name = variable_define(generator, node->variable_declaration.name);
    const char *value = emit_expression(generator, node->variable_declaration.initializer);
    emit_line(generator, "%s %s = %s;", c_type_for(generator, type), c_name, value);
}

static void emit_expression_statement_body(CodeGenerator *generator, const ASTNode *node) {
    const ASTNode *expression = node->expression_statement.expression;
    switch (expression->kind) {
    case NODE_IF:
        emit_if(generator, expression, NULL, false);
        break;
    case NODE_ASSIGN:
    case NODE_COMPOUND_ASSIGN:
        emit_statement(generator, expression);
        break;
    default: {
        const char *value = emit_expression(generator, expression);
        emit_line(generator, "%s;", value);
        break;
    }
    }
}

static void emit_assign_statement(CodeGenerator *generator, const ASTNode *node) {
    const char *target;
    if (node->assign.target->kind == NODE_IDENTIFIER) {
        target = variable_lookup(generator, node->assign.target->identifier.name);
    } else if (node->assign.target->kind == NODE_INDEX) {
        const char *obj = emit_expression(generator, node->assign.target->index_access.object);
        const char *idx = emit_expression(generator, node->assign.target->index_access.index);
        target = arena_sprintf(generator->arena, "%s._data[%s]", obj, idx);
    } else {
        target = emit_expression(generator, node->assign.target);
    }
    const char *value = emit_expression(generator, node->assign.value);
    emit_line(generator, "%s = %s;", target, value);
}

static void emit_compound_assign_statement(CodeGenerator *generator, const ASTNode *node) {
    const char *target;
    if (node->compound_assign.target->kind == NODE_IDENTIFIER) {
        target = variable_lookup(generator, node->compound_assign.target->identifier.name);
    } else {
        target = emit_expression(generator, node->compound_assign.target);
    }
    const char *value = emit_expression(generator, node->compound_assign.value);
    emit_line(generator, "%s %s %s;", target, c_compound_operator(node->compound_assign.op), value);
}

static void emit_loop_statement(CodeGenerator *generator, const ASTNode *node) {
    emit_line(generator, "while (1) {");
    generator->indent++;
    const ASTNode *body = node->loop.body;
    if (body != NULL && body->kind == NODE_BLOCK) {
        emit_block_statements(generator, body);
        if (body->block.result != NULL) {
            emit_statement(generator, body->block.result);
        }
    }
    generator->indent--;
    emit_line(generator, "}");
}

static void emit_for_statement(CodeGenerator *generator, const ASTNode *node) {
    const char *start = emit_expression(generator, node->for_loop.start);
    const char *end = emit_expression(generator, node->for_loop.end);
    const char *c_variable_name = variable_define(generator, node->for_loop.variable_name);
    emit_line(generator, "for (int32_t %s = %s; %s < %s; %s++) {", c_variable_name, start, c_variable_name, end,
              c_variable_name);
    generator->indent++;
    if (node->for_loop.body != NULL && node->for_loop.body->kind == NODE_BLOCK) {
        emit_block_statements(generator, node->for_loop.body);
        if (node->for_loop.body->block.result != NULL) {
            emit_statement(generator, node->for_loop.body->block.result);
        }
    }
    generator->indent--;
    emit_line(generator, "}");
}

/** Statement dispatch - emits a C statement for @p node. */
static void emit_statement(CodeGenerator *generator, const ASTNode *node) {
    if (node == NULL) {
        return;
    }
    switch (node->kind) {
    case NODE_VARIABLE_DECLARATION:
        emit_variable_declaration_statement(generator, node);
        break;
    case NODE_EXPRESSION_STATEMENT:
        emit_expression_statement_body(generator, node);
        break;
    case NODE_ASSIGN:
        emit_assign_statement(generator, node);
        break;
    case NODE_COMPOUND_ASSIGN:
        emit_compound_assign_statement(generator, node);
        break;
    case NODE_BREAK:
        emit_line(generator, "break;");
        break;
    case NODE_CONTINUE:
        emit_line(generator, "continue;");
        break;
    case NODE_LOOP:
        emit_loop_statement(generator, node);
        break;
    case NODE_FOR:
        emit_for_statement(generator, node);
        break;
    case NODE_IF:
        emit_if(generator, node, NULL, false);
        break;
    case NODE_BLOCK:
        emit_line(generator, "{");
        generator->indent++;
        emit_block_statements(generator, node);
        if (node->block.result != NULL) {
            emit_statement(generator, node->block.result);
        }
        generator->indent--;
        emit_line(generator, "}");
        break;
    default: {
        const char *value = emit_expression(generator, node);
        emit_line(generator, "%s;", value);
        break;
    }
    }
}

/**
 * Emit the body of a function: block statements, trailing result
 * expression, and an implicit `return 0;` for main.
 */
static void emit_function_body(CodeGenerator *generator, const ASTNode *function_node) {
    const ASTNode *body = function_node->function_declaration.body;
    const Type *return_type = function_node->type;
    bool is_unit = return_type == NULL || return_type->kind == TYPE_UNIT;
    bool is_main = strcmp(function_node->function_declaration.name, "main") == 0;

    // Register parameters in variable tracking
    for (int32_t i = 0; i < BUFFER_LENGTH(function_node->function_declaration.parameters); i++) {
        const ASTNode *parameter = function_node->function_declaration.parameters[i];
        variable_define(generator, parameter->parameter.name);
    }

    if (body->kind == NODE_BLOCK) {
        // Block body with optional trailing result
        emit_block_statements(generator, body);

        if (body->block.result != NULL) {
            const ASTNode *result_node = body->block.result;
            // Statement-like results need special handling
            if (result_node->kind == NODE_ASSIGN || result_node->kind == NODE_COMPOUND_ASSIGN) {
                // Emit as statement (side-effect only in function body)
                emit_statement(generator, result_node);
            } else if (!is_unit && !is_main) {
                const char *result = emit_expression(generator, result_node);
                emit_line(generator, "return %s;", result);
            } else {
                // Unit/main: evaluate for side effects
                if (result_node->kind == NODE_CALL) {
                    const char *result = emit_expression(generator, result_node);
                    emit_line(generator, "%s;", result);
                } else if (result_node->kind == NODE_IF) {
                    emit_if(generator, result_node, NULL, false);
                } else {
                    const char *result = emit_expression(generator, result_node);
                    emit_line(generator, "(void)%s;", result);
                }
            }
        }
    } else {
        // Expression body (fn foo() = expr)
        const char *result = emit_expression(generator, body);
        if (!is_unit && !is_main) {
            emit_line(generator, "return %s;", result);
        } else {
            emit_line(generator, "(void)%s;", result);
        }
    }
}

static void emit_function_declaration(CodeGenerator *generator, const ASTNode *node, bool forward_only) {
    bool is_public = node->function_declaration.is_public;
    bool is_main = strcmp(node->function_declaration.name, "main") == 0;

    // Return type
    const char *return_type = is_main ? "int" : c_type_for(generator, node->type);

    // Static prefix for non-public, non-main
    const char *prefix = (!is_public && !is_main) ? "static " : "";

    // Mangled function name
    const char *function_name = mangle_function_name(generator, node->function_declaration.name);

    // Parameters
    emit_indent(generator);
    fprintf(generator->output, "%s%s %s(", prefix, return_type, function_name);

    int32_t parameter_count = BUFFER_LENGTH(node->function_declaration.parameters);
    if (parameter_count == 0) {
        fprintf(generator->output, "void");
    } else {
        for (int32_t i = 0; i < parameter_count; i++) {
            const ASTNode *parameter = node->function_declaration.parameters[i];
            const Type *parameter_type = parameter->type;
            if (parameter_type == NULL && parameter->parameter.type.kind == AST_TYPE_NAME) {
                parameter_type = type_from_name(parameter->parameter.type.name);
            }
            if (parameter_type == NULL) {
                parameter_type = &TYPE_I32_INSTANCE;
            }
            if (i > 0) {
                fprintf(generator->output, ", ");
            }
            fprintf(generator->output, "%s %s", c_type_for(generator, parameter_type), parameter->parameter.name);
        }
    }

    if (forward_only) {
        fprintf(generator->output, ");\n");
        return;
    }

    fprintf(generator->output, ") {\n");
    generator->indent++;
    variable_scope_reset(generator);
    emit_function_body(generator, node);

    if (is_main) {
        emit_line(generator, "return 0;");
    }

    generator->indent--;
    emit_line(generator, "}");
    fprintf(generator->output, "\n");
}

/** Emit the preamble: generated-file warning and required C headers. */
static void emit_preamble(CodeGenerator *generator) {
    emit(generator, "// Generated by resurg compiler - do not edit.\n");
    emit(generator, "#include <stdint.h>\n");
    emit(generator, "#include <stdbool.h>\n");
    emit(generator, "#include \"runtime.h\"\n\n");
}

/**
 * Emit the full C translation unit: preamble, source-file constant, module
 * comment, forward declarations, function definitions, and (if present) a
 * `_rsg_top_level()` wrapper for top-level statements.
 */
static void emit_file(CodeGenerator *generator, const ASTNode *file) {
    emit_preamble(generator);

    // Emit source file path constant (used by rsg_assert)
    generator->source_file = c_escape_file_path(generator, file->location.file);
    emit(generator, "static const char *_rsg_file = \"%s\";\n\n", generator->source_file);

    // Emit module comment
    for (int32_t i = 0; i < BUFFER_LENGTH(file->file.declarations); i++) {
        const ASTNode *declaration = file->file.declarations[i];
        if (declaration->kind == NODE_MODULE) {
            generator->module = declaration->module.name;
            emit(generator, "// module %s\n\n", declaration->module.name);
        }
    }

    // Collect and emit compound type definitions (arrays, tuples)
    if (g_compound_types != NULL) {
        BUFFER_FREE(g_compound_types);
        g_compound_types = NULL;
    }
    collect_compound_types(file);
    emit_compound_typedefs(generator);

    // Forward declarations for all functions
    for (int32_t i = 0; i < BUFFER_LENGTH(file->file.declarations); i++) {
        const ASTNode *declaration = file->file.declarations[i];
        if (declaration->kind == NODE_FUNCTION_DECLARATION) {
            emit_function_declaration(generator, declaration, true);
        }
    }
    emit(generator, "\n");

    // Full function definitions
    for (int32_t i = 0; i < BUFFER_LENGTH(file->file.declarations); i++) {
        const ASTNode *declaration = file->file.declarations[i];
        if (declaration->kind == NODE_FUNCTION_DECLARATION) {
            emit_function_declaration(generator, declaration, false);
        }
    }

    // Top-level statements (outside functions) - wrap in a helper if needed
    bool has_top_statements = false;
    for (int32_t i = 0; i < BUFFER_LENGTH(file->file.declarations); i++) {
        const ASTNode *declaration = file->file.declarations[i];
        if (declaration->kind != NODE_MODULE && declaration->kind != NODE_FUNCTION_DECLARATION &&
            declaration->kind != NODE_TYPE_ALIAS) {
            has_top_statements = true;
            break;
        }
    }
    if (has_top_statements) {
        emit(generator, "static void _rsg_top_level(void) {\n");
        generator->indent++;
        for (int32_t i = 0; i < BUFFER_LENGTH(file->file.declarations); i++) {
            const ASTNode *declaration = file->file.declarations[i];
            if (declaration->kind != NODE_MODULE && declaration->kind != NODE_FUNCTION_DECLARATION &&
                declaration->kind != NODE_TYPE_ALIAS) {
                emit_statement(generator, declaration);
            }
        }
        generator->indent--;
        emit(generator, "}\n\n");
    }
}

CodeGenerator *code_generator_create(FILE *output, Arena *arena) {
    CodeGenerator *generator = malloc(sizeof(*generator));
    if (generator == NULL) {
        rsg_fatal("out of memory");
    }
    generator->output = output;
    generator->arena = arena;
    generator->indent = 0;
    generator->module = NULL;
    generator->source_file = NULL;
    generator->temporary_counter = 0;
    generator->string_builder_counter = 0;
    generator->variables = NULL;
    generator->shadow_variable_counter = 0;
    return generator;
}

void code_generator_destroy(CodeGenerator *generator) {
    if (generator != NULL) {
        BUFFER_FREE(generator->variables);
        free(generator);
    }
}

void code_generator_emit(CodeGenerator *generator, const ASTNode *file) {
    emit_file(generator, file);
}
