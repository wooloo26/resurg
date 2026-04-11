#include "rsg_rand.h"

// ── xoshiro256** state ────────────────────────────────────────────────

static uint64_t g_rng_state[4] = {
    0x853c49e6748fea9bULL,
    0xda3e39cb94b95bdbULL,
    0x5b5ad4ceb667f385ULL,
    0x9266677dc26f26e8ULL,
};

static uint64_t rotl(uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

static uint64_t xoshiro_next(void) {
    uint64_t *s = g_rng_state;
    uint64_t result = rotl(s[1] * 5, 7) * 9;
    uint64_t t = s[1] << 17;

    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl(s[3], 45);

    return result;
}

// ── SplitMix64 for seeding ────────────────────────────────────────────

static uint64_t splitmix64(uint64_t *state) {
    uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

// ── Public API ────────────────────────────────────────────────────────

void rsg_rand_seed(uint64_t s) {
    g_rng_state[0] = splitmix64(&s);
    g_rng_state[1] = splitmix64(&s);
    g_rng_state[2] = splitmix64(&s);
    g_rng_state[3] = splitmix64(&s);
}

int32_t rsg_rand_rand_i32(int32_t lo, int32_t hi) {
    if (lo >= hi) {
        return lo;
    }
    uint64_t r = xoshiro_next();
    uint32_t range = (uint32_t)(hi - lo + 1);
    return lo + (int32_t)(r % range);
}

double rsg_rand_rand_f64(void) {
    uint64_t r = xoshiro_next();
    return (double)(r >> 11) * 0x1.0p-53;
}
