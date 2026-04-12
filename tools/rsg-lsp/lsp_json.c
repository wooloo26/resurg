#include "lsp_json.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @file lsp_json.c
 * @brief Minimal recursive-descent JSON parser and growable JSON builder.
 */

// ── Parser internals ───────────────────────────────────────────────────

typedef struct {
    const char *src;
    size_t pos;
} JsonParser;

static void jp_skip_ws(JsonParser *p) {
    while (p->src[p->pos] == ' ' || p->src[p->pos] == '\t' || p->src[p->pos] == '\n' ||
           p->src[p->pos] == '\r') {
        p->pos++;
    }
}

static char jp_peek(JsonParser *p) {
    jp_skip_ws(p);
    return p->src[p->pos];
}

static bool jp_match(JsonParser *p, const char *lit) {
    jp_skip_ws(p);
    size_t len = strlen(lit);
    if (strncmp(p->src + p->pos, lit, len) == 0) {
        p->pos += len;
        return true;
    }
    return false;
}

static JsonValue *json_alloc(JsonKind kind) {
    JsonValue *v = calloc(1, sizeof(JsonValue));
    if (v == NULL) {
        return NULL;
    }
    v->kind = kind;
    return v;
}

static JsonValue *jp_parse_value(JsonParser *p);

static JsonValue *jp_parse_string(JsonParser *p) {
    jp_skip_ws(p);
    if (p->src[p->pos] != '"') {
        return NULL;
    }
    p->pos++; // consume opening "

    // First pass: compute length.
    size_t start = p->pos;
    size_t len = 0;
    while (p->src[p->pos] != '\0' && p->src[p->pos] != '"') {
        if (p->src[p->pos] == '\\') {
            p->pos++;
            if (p->src[p->pos] == 'u') {
                p->pos += 4; // skip 4 hex digits
            }
        }
        p->pos++;
        len++;
    }
    if (p->src[p->pos] != '"') {
        return NULL;
    }
    p->pos++; // consume closing "

    // Second pass: decode escape sequences.
    char *buf = malloc(len + 1);
    if (buf == NULL) {
        return NULL;
    }
    size_t read_pos = start;
    size_t write_pos = 0;
    while (p->src[read_pos] != '\0' && read_pos < p->pos - 1) {
        if (p->src[read_pos] == '\\') {
            read_pos++;
            switch (p->src[read_pos]) {
            case '"':
                buf[write_pos++] = '"';
                break;
            case '\\':
                buf[write_pos++] = '\\';
                break;
            case '/':
                buf[write_pos++] = '/';
                break;
            case 'n':
                buf[write_pos++] = '\n';
                break;
            case 'r':
                buf[write_pos++] = '\r';
                break;
            case 't':
                buf[write_pos++] = '\t';
                break;
            case 'b':
                buf[write_pos++] = '\b';
                break;
            case 'f':
                buf[write_pos++] = '\f';
                break;
            case 'u':
                // Simplified: just copy \uXXXX as-is for now.
                buf[write_pos++] = '\\';
                buf[write_pos++] = 'u';
                for (int i = 0; i < 4 && p->src[read_pos + 1 + i] != '\0'; i++) {
                    buf[write_pos++] = p->src[read_pos + 1 + i];
                }
                read_pos += 4;
                break;
            default:
                buf[write_pos++] = p->src[read_pos];
                break;
            }
        } else {
            buf[write_pos++] = p->src[read_pos];
        }
        read_pos++;
    }
    buf[write_pos] = '\0';

    JsonValue *v = json_alloc(JSON_STRING);
    if (v == NULL) {
        free(buf);
        return NULL;
    }
    v->string.data = buf;
    v->string.len = write_pos;
    return v;
}

static JsonValue *jp_parse_number(JsonParser *p) {
    jp_skip_ws(p);
    const char *start = p->src + p->pos;
    char *end = NULL;
    double num = strtod(start, &end);
    if (end == start) {
        return NULL;
    }
    p->pos += (size_t)(end - start);
    JsonValue *v = json_alloc(JSON_NUMBER);
    if (v != NULL) {
        v->number = num;
    }
    return v;
}

static JsonValue *jp_parse_array(JsonParser *p) {
    jp_skip_ws(p);
    if (p->src[p->pos] != '[') {
        return NULL;
    }
    p->pos++; // consume [

    JsonValue *v = json_alloc(JSON_ARRAY);
    if (v == NULL) {
        return NULL;
    }

    int32_t cap = 0;
    if (jp_peek(p) == ']') {
        p->pos++;
        return v;
    }

    for (;;) {
        JsonValue *item = jp_parse_value(p);
        if (item == NULL) {
            json_free(v);
            return NULL;
        }
        // Grow array.
        if (v->array.count >= cap) {
            cap = cap == 0 ? 8 : cap * 2;
            JsonValue **tmp =
                (JsonValue **)realloc((void *)v->array.items, (size_t)cap * sizeof(JsonValue *));
            if (tmp == NULL) {
                json_free(item);
                json_free(v);
                return NULL;
            }
            v->array.items = tmp;
        }
        v->array.items[v->array.count++] = item;

        jp_skip_ws(p);
        if (p->src[p->pos] == ',') {
            p->pos++;
        } else {
            break;
        }
    }

    jp_skip_ws(p);
    if (p->src[p->pos] != ']') {
        json_free(v);
        return NULL;
    }
    p->pos++;
    return v;
}

static JsonValue *jp_parse_object(JsonParser *p) {
    jp_skip_ws(p);
    if (p->src[p->pos] != '{') {
        return NULL;
    }
    p->pos++; // consume {

    JsonValue *v = json_alloc(JSON_OBJECT);
    if (v == NULL) {
        return NULL;
    }

    int32_t cap = 0;
    if (jp_peek(p) == '}') {
        p->pos++;
        return v;
    }

    for (;;) {
        // Parse key (must be a string).
        JsonValue *key_val = jp_parse_string(p);
        if (key_val == NULL || key_val->kind != JSON_STRING) {
            json_free(key_val);
            json_free(v);
            return NULL;
        }
        char *key = key_val->string.data;
        key_val->string.data = NULL; // transfer ownership
        json_free(key_val);

        jp_skip_ws(p);
        if (p->src[p->pos] != ':') {
            free(key);
            json_free(v);
            return NULL;
        }
        p->pos++; // consume :

        JsonValue *val = jp_parse_value(p);
        if (val == NULL) {
            free(key);
            json_free(v);
            return NULL;
        }

        // Grow pairs.
        if (v->object.count >= cap) {
            cap = cap == 0 ? 8 : cap * 2;
            JsonPair *ptmp = realloc(v->object.pairs, (size_t)cap * sizeof(JsonPair));
            if (ptmp == NULL) {
                free(key);
                json_free(val);
                json_free(v);
                return NULL;
            }
            v->object.pairs = ptmp;
        }
        v->object.pairs[v->object.count].key = key;
        v->object.pairs[v->object.count].value = val;
        v->object.count++;

        jp_skip_ws(p);
        if (p->src[p->pos] == ',') {
            p->pos++;
        } else {
            break;
        }
    }

    jp_skip_ws(p);
    if (p->src[p->pos] != '}') {
        json_free(v);
        return NULL;
    }
    p->pos++;
    return v;
}

static JsonValue *jp_parse_value(JsonParser *p) {
    jp_skip_ws(p);
    char c = p->src[p->pos];

    if (c == '"') {
        return jp_parse_string(p);
    }
    if (c == '{') {
        return jp_parse_object(p);
    }
    if (c == '[') {
        return jp_parse_array(p);
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        return jp_parse_number(p);
    }
    if (jp_match(p, "true")) {
        JsonValue *v = json_alloc(JSON_BOOL);
        if (v != NULL) {
            v->boolean = true;
        }
        return v;
    }
    if (jp_match(p, "false")) {
        JsonValue *v = json_alloc(JSON_BOOL);
        if (v != NULL) {
            v->boolean = false;
        }
        return v;
    }
    if (jp_match(p, "null")) {
        return json_alloc(JSON_NULL);
    }
    return NULL;
}

// ── Public API ─────────────────────────────────────────────────────────

JsonValue *json_parse(const char *src) {
    if (src == NULL) {
        return NULL;
    }
    JsonParser parser = {.src = src, .pos = 0};
    return jp_parse_value(&parser);
}

void json_free(JsonValue *val) {
    if (val == NULL) {
        return;
    }
    switch (val->kind) {
    case JSON_STRING:
        free(val->string.data);
        break;
    case JSON_ARRAY:
        for (int32_t i = 0; i < val->array.count; i++) {
            json_free(val->array.items[i]); // NOLINT(clang-analyzer-core.CallAndMessage)
        }
        free((void *)val->array.items);
        break;
    case JSON_OBJECT:
        for (int32_t i = 0; i < val->object.count; i++) {
            free(val->object.pairs[i].key);        // NOLINT(clang-analyzer-core.CallAndMessage)
            json_free(val->object.pairs[i].value); // NOLINT(clang-analyzer-core.CallAndMessage)
        }
        free(val->object.pairs);
        break;
    default:
        break;
    }
    free(val);
}

JsonValue *json_get(const JsonValue *obj, const char *key) {
    if (obj == NULL || obj->kind != JSON_OBJECT || key == NULL) {
        return NULL;
    }
    for (int32_t i = 0; i < obj->object.count; i++) {
        if (strcmp(obj->object.pairs[i].key, key) == 0) {
            return obj->object.pairs[i].value;
        }
    }
    return NULL;
}

const char *json_str(const JsonValue *val) {
    if (val == NULL || val->kind != JSON_STRING) {
        return NULL;
    }
    return val->string.data;
}

double json_num(const JsonValue *val) {
    if (val == NULL || val->kind != JSON_NUMBER) {
        return 0.0;
    }
    return val->number;
}

int64_t json_int(const JsonValue *val) {
    if (val == NULL || val->kind != JSON_NUMBER) {
        return 0;
    }
    return (int64_t)val->number;
}

bool json_bool(const JsonValue *val) {
    if (val == NULL || val->kind != JSON_BOOL) {
        return false;
    }
    return val->boolean;
}

// ── Builder ────────────────────────────────────────────────────────────

void jbuf_init(JsonBuf *b) {
    b->cap = 1024;
    b->data = malloc(b->cap);
    b->len = 0;
    if (b->data != NULL) {
        b->data[0] = '\0';
    }
}

void jbuf_destroy(JsonBuf *b) {
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static void jbuf_ensure(JsonBuf *b, size_t extra) {
    size_t needed = b->len + extra + 1;
    if (needed <= b->cap) {
        return;
    }
    while (b->cap < needed) {
        b->cap *= 2;
    }
    char *tmp = realloc(b->data, b->cap);
    if (tmp != NULL) {
        b->data = tmp;
    }
}

static void jbuf_append(JsonBuf *b, const char *s, size_t len) {
    jbuf_ensure(b, len);
    memcpy(b->data + b->len, s, len);
    b->len += len;
    b->data[b->len] = '\0';
}

static void jbuf_putc(JsonBuf *b, char c) {
    jbuf_ensure(b, 1);
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}

const char *jbuf_str(const JsonBuf *b) {
    return b->data;
}

void jbuf_obj_start(JsonBuf *b) {
    jbuf_putc(b, '{');
}

void jbuf_obj_end(JsonBuf *b) {
    // Remove trailing comma if present.
    if (b->len > 0 && b->data[b->len - 1] == ',') {
        b->len--;
    }
    jbuf_putc(b, '}');
}

void jbuf_arr_start(JsonBuf *b) {
    jbuf_putc(b, '[');
}

void jbuf_arr_end(JsonBuf *b) {
    if (b->len > 0 && b->data[b->len - 1] == ',') {
        b->len--;
    }
    jbuf_putc(b, ']');
}

void jbuf_key(JsonBuf *b, const char *key) {
    jbuf_putc(b, '"');
    jbuf_append(b, key, strlen(key));
    jbuf_putc(b, '"');
    jbuf_putc(b, ':');
}

void jbuf_str_val(JsonBuf *b, const char *val) {
    jbuf_putc(b, '"');
    if (val != NULL) {
        for (const char *p = val; *p != '\0'; p++) {
            switch (*p) {
            case '"':
                jbuf_append(b, "\\\"", 2);
                break;
            case '\\':
                jbuf_append(b, "\\\\", 2);
                break;
            case '\n':
                jbuf_append(b, "\\n", 2);
                break;
            case '\r':
                jbuf_append(b, "\\r", 2);
                break;
            case '\t':
                jbuf_append(b, "\\t", 2);
                break;
            default:
                if ((unsigned char)*p < 0x20) {
                    char esc[8];
                    snprintf(esc, sizeof(esc), "\\u%04x", (unsigned char)*p);
                    jbuf_append(b, esc, 6);
                } else {
                    jbuf_putc(b, *p);
                }
                break;
            }
        }
    }
    jbuf_putc(b, '"');
}

void jbuf_int_val(JsonBuf *b, int64_t val) {
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%lld", (long long)val);
    jbuf_append(b, tmp, (size_t)n);
}

void jbuf_bool_val(JsonBuf *b, bool val) {
    if (val) {
        jbuf_append(b, "true", 4);
    } else {
        jbuf_append(b, "false", 5);
    }
}

void jbuf_null_val(JsonBuf *b) {
    jbuf_append(b, "null", 4);
}

void jbuf_comma(JsonBuf *b) {
    jbuf_putc(b, ',');
}

void jbuf_raw(JsonBuf *b, const char *s) {
    jbuf_append(b, s, strlen(s));
}
