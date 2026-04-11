#include "rsg_path.h"

#include <string.h>

#include "rsg_internal.h"

// ── Helpers ───────────────────────────────────────────────────────────

static bool is_sep(char c) {
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

static int32_t last_sep(RsgStr path) {
    for (int32_t i = path.len - 1; i >= 0; i--) {
        if (is_sep(path.data[i])) {
            return i;
        }
    }
    return -1;
}

// ── Module functions ──────────────────────────────────────────────────

RsgStr rsg_path_join(RsgStr a, RsgStr b) {
    if (a.len == 0) {
        return b;
    }
    if (b.len == 0) {
        return a;
    }

    bool a_has_sep = is_sep(a.data[a.len - 1]);
    bool b_has_sep = is_sep(b.data[0]);

    if (a_has_sep && b_has_sep) {
        // Skip duplicate separator
        int32_t total = a.len + b.len - 1;
        char *buf = checked_malloc(total + 1);
        memcpy(buf, a.data, a.len);
        memcpy(buf + a.len, b.data + 1, b.len - 1);
        buf[total] = '\0';
        return (RsgStr){.data = buf, .len = total, .ref_count = 1};
    }
    if (a_has_sep || b_has_sep) {
        return rsg_str_concat(a, b);
    }
    // No separator — insert one
#ifdef _WIN32
    char sep = '\\';
#else
    char sep = '/';
#endif
    int32_t total = a.len + 1 + b.len;
    char *buf = checked_malloc(total + 1);
    memcpy(buf, a.data, a.len);
    buf[a.len] = sep;
    memcpy(buf + a.len + 1, b.data, b.len);
    buf[total] = '\0';
    return (RsgStr){.data = buf, .len = total, .ref_count = 1};
}

RsgStr rsg_path_file_name(RsgStr path) {
    int32_t sep = last_sep(path);
    if (sep < 0) {
        return path;
    }
    int32_t start = sep + 1;
    if (start >= path.len) {
        return rsg_str_empty();
    }
    return rsg_str_new(path.data + start, path.len - start);
}

RsgStr rsg_path_dir(RsgStr path) {
    int32_t sep = last_sep(path);
    if (sep < 0) {
        return rsg_str_lit(".");
    }
    if (sep == 0) {
        return rsg_str_new(path.data, 1);
    }
    return rsg_str_new(path.data, sep);
}

RsgStr rsg_path_extension(RsgStr path) {
    // Find the last dot after the last separator
    int32_t sep = last_sep(path);
    int32_t dot = -1;
    for (int32_t i = path.len - 1; i > sep; i--) {
        if (path.data[i] == '.') {
            dot = i;
            break;
        }
    }
    if (dot < 0) {
        return rsg_str_empty();
    }
    return rsg_str_new(path.data + dot, path.len - dot);
}

bool rsg_path_is_absolute(RsgStr path) {
    if (path.len == 0) {
        return false;
    }
#ifdef _WIN32
    // Drive letter: C:\... or \\server
    if (path.len >= 3 && path.data[1] == ':' && is_sep(path.data[2])) {
        return true;
    }
    if (path.len >= 2 && is_sep(path.data[0]) && is_sep(path.data[1])) {
        return true;
    }
    return false;
#else
    return path.data[0] == '/';
#endif
}
