#ifndef RSG_RAND_H
#define RSG_RAND_H

/**
 * @file rsg_rand.h
 * @brief std/rand module — random number generation.
 */

#include <stdint.h>

/** Seed the global RNG. */
void rsg_rand_seed(uint64_t s);
/** Random i32 in [lo, hi]. */
int32_t rsg_rand_rand_i32(int32_t lo, int32_t hi);
/** Random f64 in [0.0, 1.0). */
double rsg_rand_rand_f64(void);

#endif // RSG_RAND_H
