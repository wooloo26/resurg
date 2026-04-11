#include "rsg_sort.h"

#include <stdlib.h>
#include <string.h>

// ── qsort comparators ────────────────────────────────────────────────

static int cmp_i32(const void *a, const void *b) {
    int32_t va = *(const int32_t *)a;
    int32_t vb = *(const int32_t *)b;
    return (va > vb) - (va < vb);
}

static int cmp_i64(const void *a, const void *b) {
    int64_t va = *(const int64_t *)a;
    int64_t vb = *(const int64_t *)b;
    return (va > vb) - (va < vb);
}

static int cmp_f64(const void *a, const void *b) {
    double va = *(const double *)a;
    double vb = *(const double *)b;
    return (va > vb) - (va < vb);
}

static int cmp_str(const void *a, const void *b) {
    const RsgStr *sa = (const RsgStr *)a;
    const RsgStr *sb = (const RsgStr *)b;
    int32_t min_len = sa->len < sb->len ? sa->len : sb->len;
    int result = memcmp(sa->data, sb->data, min_len);
    if (result != 0) {
        return result;
    }
    return (sa->len > sb->len) - (sa->len < sb->len);
}

// ── Sort functions ────────────────────────────────────────────────────

void rsg_sort_sort_i32(RsgSlice *s) {
    if (s->len > 1) {
        qsort(s->data, (size_t)s->len, sizeof(int32_t), cmp_i32);
    }
}

void rsg_sort_sort_i64(RsgSlice *s) {
    if (s->len > 1) {
        qsort(s->data, (size_t)s->len, sizeof(int64_t), cmp_i64);
    }
}

void rsg_sort_sort_f64(RsgSlice *s) {
    if (s->len > 1) {
        qsort(s->data, (size_t)s->len, sizeof(double), cmp_f64);
    }
}

void rsg_sort_sort_str(RsgSlice *s) {
    if (s->len > 1) {
        qsort(s->data, (size_t)s->len, sizeof(RsgStr), cmp_str);
    }
}

// ── Sorted checks ─────────────────────────────────────────────────────

bool rsg_sort_is_sorted_i32(RsgSlice s) {
    const int32_t *arr = (const int32_t *)s.data;
    for (int32_t i = 1; i < s.len; i++) {
        if (arr[i] < arr[i - 1]) {
            return false;
        }
    }
    return true;
}

bool rsg_sort_is_sorted_str(RsgSlice s) {
    const RsgStr *arr = (const RsgStr *)s.data;
    for (int32_t i = 1; i < s.len; i++) {
        if (cmp_str(&arr[i], &arr[i - 1]) < 0) {
            return false;
        }
    }
    return true;
}

// ── Binary search ─────────────────────────────────────────────────────

int32_t rsg_sort_binary_search_i32(RsgSlice s, int32_t target) {
    const int32_t *arr = (const int32_t *)s.data;
    int32_t lo = 0;
    int32_t hi = s.len - 1;
    while (lo <= hi) {
        int32_t mid = lo + (hi - lo) / 2;
        if (arr[mid] == target) {
            return mid;
        }
        if (arr[mid] < target) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return -1;
}
