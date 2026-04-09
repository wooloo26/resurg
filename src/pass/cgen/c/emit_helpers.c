#include "helpers.h"

// ── Output helpers ─────────────────────────────────────────────────────

/** Replace '.' with '_' for valid C identifiers. */
static const char *mangle_dots(Arena *arena, const char *name) {
    size_t len = strlen(name);
    char *buf = arena_alloc(arena, len + 1);
    for (size_t i = 0; i < len; i++) {
        buf[i] = (char)((name[i] == '.') ? '_' : name[i]);
    }
    buf[len] = '\0';
    return buf;
}

void emit_indent(CGen *cgen) {
    for (int32_t i = 0; i < cgen->indent; i++) {
        fprintf(cgen->output, "    ");
    }
}

void emit(CGen *cgen, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(cgen->output, fmt, args);
    va_end(args);
}

void emit_line(CGen *cgen, const char *fmt, ...) {
    emit_indent(cgen);
    va_list args;
    va_start(args, fmt);
    vfprintf(cgen->output, fmt, args);
    va_end(args);
    fprintf(cgen->output, "\n");
}

const char *next_temp(CGen *cgen) {
    return arena_sprintf(cgen->arena, "_rsg_tmp_%d", cgen->temp_counter++);
}

const char *next_str_builder(CGen *cgen) {
    return arena_sprintf(cgen->arena, "_rsg_sb_%d", cgen->str_builder_counter++);
}

// ── C fmtting helpers ───────────────────────────────────────────────

const char *c_str_escape(const CGen *cgen, const char *src, int32_t src_len) {
    if (src == NULL) {
        return "";
    }
    // Count extra space needed for escapes
    int32_t extra = 0;
    for (int32_t i = 0; i < src_len; i++) {
        switch (src[i]) {
        case '\\':
        case '"':
        case '\n':
        case '\r':
        case '\t':
        case '\0':
            extra++;
            break;
        default:
            break;
        }
    }
    if (extra == 0) {
        return src;
    }
    char *buf = arena_alloc(cgen->arena, src_len + extra + 1);
    int32_t write_idx = 0;
    for (int32_t i = 0; i < src_len; i++) {
        switch (src[i]) {
        case '\\':
            buf[write_idx++] = '\\';
            buf[write_idx++] = '\\';
            break;
        case '"':
            buf[write_idx++] = '\\';
            buf[write_idx++] = '"';
            break;
        case '\n':
            buf[write_idx++] = '\\';
            buf[write_idx++] = 'n';
            break;
        case '\r':
            buf[write_idx++] = '\\';
            buf[write_idx++] = 'r';
            break;
        case '\t':
            buf[write_idx++] = '\\';
            buf[write_idx++] = 't';
            break;
        case '\0':
            buf[write_idx++] = '\\';
            buf[write_idx++] = '0';
            break;
        default:
            buf[write_idx++] = src[i];
            break;
        }
    }
    buf[write_idx] = '\0';
    return buf;
}

/** Ensure float representation has decimal notation: append ".0" if missing. */
static void ensure_float_decimal(char *buf, int32_t *len) {
    for (int32_t i = 0; i < *len; i++) {
        if (buf[i] == '.' || buf[i] == 'e' || buf[i] == 'E') {
            return;
        }
    }
    buf[(*len)++] = '.';
    buf[(*len)++] = '0';
    buf[*len] = '\0';
}

static const char *fmt_float(const CGen *cgen, double value, int32_t precision,
                             const char *suffix) {
    char buf[64];
    int32_t len = snprintf(buf, sizeof(buf), "%.*g", precision, value);
    ensure_float_decimal(buf, &len);
    if (suffix[0] != '\0') {
        buf[len++] = suffix[0];
        buf[len] = '\0';
    }
    return arena_strdup(cgen->arena, buf);
}

const char *fmt_float64(const CGen *cgen, double value) {
    return fmt_float(cgen, value, 17, "");
}

const char *fmt_float32(const CGen *cgen, double value) {
    return fmt_float(cgen, value, 9, "f");
}

const char *c_char_escape(const CGen *cgen, uint32_t c) {
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
            return arena_sprintf(cgen->arena, "'%c'", (char)c);
        }
        return arena_sprintf(cgen->arena, "0x%04XU", c);
    }
}

// ── Type naming ────────────────────────────────────────────────────────

/** Generate a clean C id suffix for @p type. */
static const char *type_tag(CGen *gen, const Type *type) {
    switch (type->kind) {
    case TYPE_ARRAY:
        return arena_sprintf(gen->arena, "Arr_%s_%d", type_tag(gen, type->array.elem),
                             type->array.size);
    case TYPE_SLICE:
        return arena_sprintf(gen->arena, "Slice_%s", type_tag(gen, type->slice.elem));
    case TYPE_TUPLE: {
        const char *result = "Tup";
        for (int32_t i = 0; i < type->tuple.count; i++) {
            result =
                arena_sprintf(gen->arena, "%s_%s", result, type_tag(gen, type->tuple.elems[i]));
        }
        return result;
    }
    case TYPE_ERR:
        return "err";
    case TYPE_STRUCT:
        return arena_sprintf(gen->arena, "_%s", mangle_dots(gen->arena, type->struct_type.name));
    case TYPE_ENUM:
        return arena_sprintf(gen->arena, "Enum_%s", mangle_dots(gen->arena, type->enum_type.name));
    case TYPE_FN:
        return "RsgFn";
    default:
        return type_name(gen->arena, type);
    }
}

const char *c_type_for(CGen *gen, const Type *type) {
    if (type == NULL) {
        return "/* ? */";
    }
    if (type->kind == TYPE_ARRAY || type->kind == TYPE_TUPLE) {
        return arena_sprintf(gen->arena, "_Rsg%s", type_tag(gen, type));
    }
    if (type->kind == TYPE_SLICE) {
        return "RsgSlice";
    }
    if (type->kind == TYPE_STRUCT) {
        return arena_sprintf(gen->arena, "_Rsg_%s",
                             mangle_dots(gen->arena, type->struct_type.name));
    }
    if (type->kind == TYPE_ENUM) {
        return arena_sprintf(gen->arena, "_RsgEnum_%s",
                             mangle_dots(gen->arena, type->enum_type.name));
    }
    if (type->kind == TYPE_PTR) {
        return arena_sprintf(gen->arena, "%s *", c_type_for(gen, type->ptr.pointee));
    }
    return c_type_str(type);
}
