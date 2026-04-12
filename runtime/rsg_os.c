#ifndef _WIN32
// NOLINTNEXTLINE(readability-identifier-naming)
#define _POSIX_C_SOURCE 200112L
#endif

#include "rsg_os.h"

#include <stdlib.h>
#include <string.h>

#include "rsg_gc.h"
#include "rsg_internal.h"

// ── Stored argc/argv ──────────────────────────────────────────────────

static int g_argc;
static char **g_argv;

void rsg_os_init(int argc, char **argv) {
    g_argc = argc;
    g_argv = argv;
}

// ── Module functions ──────────────────────────────────────────────────

RsgSlice rsg_os_args(void) {
    // Skip argv[0] (program name)
    int32_t count = g_argc > 1 ? (int32_t)(g_argc - 1) : 0;

    RsgStr *data = rsg_heap_alloc((size_t)count * sizeof(RsgStr));
    for (int32_t i = 0; i < count; i++) {
        data[i] = rsg_str_lit(g_argv[i + 1]);
    }
    return (RsgSlice){.data = data, .len = count};
}

RsgStr rsg_os_env(RsgStr key) {
    char *ckey = checked_malloc(key.len + 1);
    memcpy(ckey, key.data, key.len);
    ckey[key.len] = '\0';

    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const char *val = getenv(ckey);
    free(ckey);

    if (val == NULL) {
        return rsg_str_empty();
    }
    return rsg_str_lit(val);
}

void rsg_os_set_env(RsgStr key, RsgStr value) {
    char *ckey = checked_malloc(key.len + 1);
    memcpy(ckey, key.data, key.len);
    ckey[key.len] = '\0';

    char *cval = checked_malloc(value.len + 1);
    memcpy(cval, value.data, value.len);
    cval[value.len] = '\0';

#ifdef _WIN32
    _putenv_s(ckey, cval);
#else
    setenv(ckey, cval, 1); // NOLINT(concurrency-mt-unsafe)
#endif

    free(ckey);
    free(cval);
}

void rsg_os_exit(int32_t code) {
    exit(code); // NOLINT(concurrency-mt-unsafe)
}
