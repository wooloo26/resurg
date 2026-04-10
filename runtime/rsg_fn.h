#ifndef RSG_FN_H
#define RSG_FN_H

/**
 * @file rsg_fn.h
 * @brief First-class function value (closure) type.
 */

/**
 * Fat pointer for first-class function values (fn types).
 * @c fn is the actual function (cast at call site to the correct
 * signature with an extra leading @c void* env param).
 * @c env holds captured state (NULL for plain function references
 * and non-capturing closures).
 */
typedef struct {
    void (*fn)(void);
    void *env;
} RsgFn;

#endif // RSG_FN_H
