#ifndef RSG_TIME_H
#define RSG_TIME_H

/**
 * @file rsg_time.h
 * @brief std/time module — time measurement.
 */

#include <stdint.h>

/** Monotonic nanosecond timestamp (for measuring durations). */
int64_t rsg_time_now_ns(void);
/** Wall-clock seconds since Unix epoch. */
int64_t rsg_time_unix_secs(void);
/** Sleep for the given number of milliseconds. */
void rsg_time_sleep_ms(int64_t ms);

#endif // RSG_TIME_H
