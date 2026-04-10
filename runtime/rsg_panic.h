#ifndef RSG_PANIC_H
#define RSG_PANIC_H

/**
 * @file rsg_panic.h
 * @brief Panic / recover runtime and assert.
 */

#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>

/** Recovery frame pushed by each function with defers. */
typedef struct RsgPanicFrame {
    jmp_buf env;
    struct RsgPanicFrame *prev;
} RsgPanicFrame;

/** Push a recovery frame onto the panic stack. */
void rsg_panic_push(RsgPanicFrame *frame);

/** Pop the top recovery frame from the panic stack. */
void rsg_panic_pop(void);

/**
 * Trigger a panic with @p msg.  Unwinds to the nearest recovery
 * frame via longjmp, or prints to stderr and exits if none.
 */
/**
 * Assert @p cond.  On failure, panics with @p msg (or a default
 * message including @p file and @p line).
 */
void rsg_assert(bool cond, const char *msg, const char *file, int32_t line);

void rsgu_panic(const char *msg);

/**
 * In a defer body, catch an active panic and return its message.
 * Returns NULL if no panic is active.  Clears the panic state.
 */
const char *rsgu_recover(void);

/** Return true if a panic is currently active (not yet recovered). */
bool rsg_is_panicking(void);

/** Re-trigger the current panic (propagate to parent frame or exit). */
void rsg_repanic(void);

#endif // RSG_PANIC_H
