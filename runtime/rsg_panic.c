#include "rsg_panic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ── Panic / Recover ───────────────────────────────────────────────────

static RsgPanicFrame *g_rsg_panic_stack = NULL;
static char *g_rsg_panic_msg = NULL;
static bool g_rsg_panicking = false;

void rsg_panic_push(RsgPanicFrame *frame) {
    frame->prev = g_rsg_panic_stack;
    g_rsg_panic_stack = frame;
}

void rsg_panic_pop(void) {
    if (g_rsg_panic_stack != NULL) {
        g_rsg_panic_stack = g_rsg_panic_stack->prev;
    }
}

void rsg_assert(bool cond, const char *msg, const char *file, int32_t line) {
    if (!cond) {
        if (msg != NULL) {
            rsgu_panic(msg);
        } else {
            // Build "assertion failed at file:line"
            char buf[512];
            snprintf(buf, sizeof(buf), "assertion failed at %s:%d", file, line);
            rsgu_panic(buf);
        }
    }
}

void rsgu_panic(const char *msg) {
    free(g_rsg_panic_msg);
    g_rsg_panic_msg = NULL;
    if (msg != NULL) {
        size_t len = strlen(msg);
        g_rsg_panic_msg = (char *)malloc(len + 1);
        if (g_rsg_panic_msg != NULL) {
            memcpy(g_rsg_panic_msg, msg, len + 1);
        }
    }
    g_rsg_panicking = true;

    if (g_rsg_panic_stack != NULL) {
        longjmp(g_rsg_panic_stack->env, 1);
    }

    // No recovery frame — print and exit
    fprintf(stderr, "panic: %s\n", msg != NULL ? msg : "(nil)");
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    exit(1);
}

const char *rsgu_recover(void) {
    if (!g_rsg_panicking) {
        return NULL;
    }
    g_rsg_panicking = false;
    return g_rsg_panic_msg;
}

bool rsg_is_panicking(void) {
    return g_rsg_panicking;
}

void rsg_repanic(void) {
    if (g_rsg_panic_stack != NULL) {
        longjmp(g_rsg_panic_stack->env, 1);
    }
    fprintf(stderr, "panic: %s\n", g_rsg_panic_msg != NULL ? g_rsg_panic_msg : "(nil)");
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    exit(1);
}
