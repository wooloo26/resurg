#ifndef RSG_LSP_JSON_H
#define RSG_LSP_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file lsp_json.h
 * @brief Minimal JSON parser and builder for the LSP server.
 *
 * Parses JSON values into a simple tagged tree and provides a growable
 * buffer for constructing JSON output.
 */

// ── JSON value types ───────────────────────────────────────────────────

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
} JsonKind;

typedef struct JsonValue JsonValue;

/** A key-value pair in a JSON object. */
typedef struct {
    char *key;
    JsonValue *value;
} JsonPair;

struct JsonValue {
    JsonKind kind;
    union {
        bool boolean;
        double number;
        struct {
            char *data;
            size_t len;
        } string;
        struct {
            JsonValue **items; // heap array
            int32_t count;
        } array;
        struct {
            JsonPair *pairs; // heap array
            int32_t count;
        } object;
    };
};

// ── Parsing ────────────────────────────────────────────────────────────

/** Parse a NUL-terminated JSON string. Returns NULL on error. Caller owns result. */
JsonValue *json_parse(const char *src);

/** Free a JsonValue tree recursively. */
void json_free(JsonValue *val);

// ── Accessors ──────────────────────────────────────────────────────────

/** Look up an object member by key. Returns NULL if not found or not an object. */
JsonValue *json_get(const JsonValue *obj, const char *key);

/** Return the string data, or NULL if not a string. */
const char *json_str(const JsonValue *val);

/** Return the number, or 0.0 if not a number. */
double json_num(const JsonValue *val);

/** Return the int value, or 0 if not a number. */
int64_t json_int(const JsonValue *val);

/** Return the bool value, or false if not a bool. */
bool json_bool(const JsonValue *val);

// ── Builder ────────────────────────────────────────────────────────────

/** Growable buffer for constructing JSON. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} JsonBuf;

void jbuf_init(JsonBuf *b);
void jbuf_destroy(JsonBuf *b);

/** Return the finished NUL-terminated string (ownership stays with buf). */
const char *jbuf_str(const JsonBuf *b);

void jbuf_obj_start(JsonBuf *b);
void jbuf_obj_end(JsonBuf *b);
void jbuf_arr_start(JsonBuf *b);
void jbuf_arr_end(JsonBuf *b);
void jbuf_key(JsonBuf *b, const char *key);
void jbuf_str_val(JsonBuf *b, const char *val);
void jbuf_int_val(JsonBuf *b, int64_t val);
void jbuf_bool_val(JsonBuf *b, bool val);
void jbuf_null_val(JsonBuf *b);
void jbuf_comma(JsonBuf *b);
void jbuf_raw(JsonBuf *b, const char *s);

#endif // RSG_LSP_JSON_H
