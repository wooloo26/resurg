#include "rsg_time.h"

#include <time.h>

#ifdef _WIN32
#include <windows.h>

int64_t rsg_time_now_ns(void) {
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    // Convert to nanoseconds: counter * 1e9 / freq
    return (int64_t)((double)counter.QuadPart / (double)freq.QuadPart * 1e9);
}

void rsg_time_sleep_ms(int64_t ms) {
    if (ms > 0) {
        Sleep((DWORD)ms);
    }
}

#else
#include <unistd.h>

int64_t rsg_time_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

void rsg_time_sleep_ms(int64_t ms) {
    if (ms <= 0) {
        return;
    }
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

#endif

int64_t rsg_time_unix_secs(void) {
    return (int64_t)time(NULL);
}
