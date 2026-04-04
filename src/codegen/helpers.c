#include "codegen/_codegen.h"

// ── Output helpers ─────────────────────────────────────────────────────

void codegen_emit_indent(CodeGenerator *generator) {
    for (int32_t i = 0; i < generator->indent; i++) {
        fprintf(generator->output, "    ");
    }
}

void codegen_emit(CodeGenerator *generator, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    vfprintf(generator->output, format, arguments);
    va_end(arguments);
}

void codegen_emit_line(CodeGenerator *generator, const char *format, ...) {
    codegen_emit_indent(generator);
    va_list arguments;
    va_start(arguments, format);
    vfprintf(generator->output, format, arguments);
    va_end(arguments);
    fprintf(generator->output, "\n");
}

const char *codegen_next_temporary(CodeGenerator *generator) {
    return arena_sprintf(generator->arena, "_rsg_tmp_%d", generator->temporary_counter++);
}

const char *codegen_next_string_builder(CodeGenerator *generator) {
    return arena_sprintf(generator->arena, "_rsg_sb_%d", generator->string_builder_counter++);
}

// ── Variable name tracking ─────────────────────────────────────────────

static int32_t variable_find(const CodeGenerator *generator, const char *name) {
    for (int32_t i = BUFFER_LENGTH(generator->variables) - 1; i >= 0; i--) {
        if (strcmp(generator->variables[i].rsg_name, name) == 0) {
            return i;
        }
    }
    return -1;
}

const char *codegen_variable_lookup(const CodeGenerator *generator, const char *name) {
    int32_t index = variable_find(generator, name);
    return (index >= 0) ? generator->variables[index].c_name : name;
}

const char *codegen_variable_define(CodeGenerator *generator, const char *name) {
    bool already_defined = (variable_find(generator, name) >= 0);
    const char *c_name = already_defined ? arena_sprintf(generator->arena, "%s__%d", name,
                                                         generator->shadow_variable_counter++)
                                         : name;
    VariableEntry entry = {name, c_name};
    BUFFER_PUSH(generator->variables, entry);
    return c_name;
}

void codegen_variable_scope_reset(CodeGenerator *generator) {
    if (generator->variables != NULL) {
        BUFFER_FREE(generator->variables);
        generator->variables = NULL;
    }
    generator->shadow_variable_counter = 0;
}

const char *codegen_mangle_function_name(CodeGenerator *generator, const char *name) {
    return arena_sprintf(generator->arena, "rsgu_%s", name);
}

// ── C formatting helpers ───────────────────────────────────────────────

const char *codegen_c_binary_operator(TokenKind op) {
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

const char *codegen_c_compound_operator(TokenKind op) {
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

const char *codegen_c_string_escape(const CodeGenerator *generator, const char *source) {
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

const char *codegen_c_escape_file_path(const CodeGenerator *generator, const char *path) {
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

static const char *format_float(const CodeGenerator *generator, double value, int32_t precision,
                                const char *suffix) {
    char buffer[64];
    int32_t length = snprintf(buffer, sizeof(buffer), "%.*g", precision, value);
    bool has_dot = false;
    for (int32_t i = 0; i < length; i++) {
        if (buffer[i] == '.' || buffer[i] == 'e' || buffer[i] == 'E') {
            has_dot = true;
            break;
        }
    }
    if (!has_dot) {
        buffer[length++] = '.';
        buffer[length++] = '0';
        buffer[length] = '\0';
    }
    if (suffix[0] != '\0') {
        buffer[length++] = suffix[0];
        buffer[length] = '\0';
    }
    return arena_strdup(generator->arena, buffer);
}

const char *codegen_format_float64(const CodeGenerator *generator, double value) {
    return format_float(generator, value, 17, "");
}

const char *codegen_format_float32(const CodeGenerator *generator, double value) {
    return format_float(generator, value, 9, "f");
}

const char *codegen_c_char_escape(const CodeGenerator *generator, uint32_t c) {
    switch (c) {
    case '\n':
        return "'\\n'";
    case '\t':
        return "'\\t'";
    case '\\':
        return "'\\\\'";
    case '\'':
        return "'\\''";
    case '\0':
        return "'\\0'";
    default:
        if (c < 128) {
            return arena_sprintf(generator->arena, "'%c'", (char)c);
        }
        return arena_sprintf(generator->arena, "0x%04XU", c);
    }
}

// ── Type naming ────────────────────────────────────────────────────────

/** Generate a clean C identifier suffix for @p type. */
static const char *type_tag(CodeGenerator *gen, const Type *type) {
    switch (type->kind) {
    case TYPE_ARRAY:
        return arena_sprintf(gen->arena, "Arr_%s_%d", type_tag(gen, type->array.element),
                             type->array.size);
    case TYPE_TUPLE: {
        const char *result = "Tup";
        for (int32_t i = 0; i < type->tuple.count; i++) {
            result =
                arena_sprintf(gen->arena, "%s_%s", result, type_tag(gen, type->tuple.elements[i]));
        }
        return result;
    }
    case TYPE_ERROR:
        return "err";
    default:
        return type_name(gen->arena, type);
    }
}

const char *codegen_c_type_for(CodeGenerator *gen, const Type *type) {
    if (type == NULL) {
        return "/* ? */";
    }
    if (type->kind == TYPE_ARRAY || type->kind == TYPE_TUPLE) {
        return arena_sprintf(gen->arena, "_Rsg%s", type_tag(gen, type));
    }
    return c_type_string(type);
}
