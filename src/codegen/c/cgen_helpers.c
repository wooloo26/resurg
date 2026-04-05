#include "_codegen.h"

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

// ── C formatting helpers ───────────────────────────────────────────────

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
    case TYPE_STRUCT:
        return arena_sprintf(gen->arena, "_%s", type->struct_type.name);
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
    if (type->kind == TYPE_STRUCT) {
        return arena_sprintf(gen->arena, "_Rsg_%s", type->struct_type.name);
    }
    if (type->kind == TYPE_POINTER) {
        return arena_sprintf(gen->arena, "%s *", codegen_c_type_for(gen, type->pointer.pointee));
    }
    return c_type_string(type);
}
