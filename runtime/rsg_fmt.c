#include "rsg_fmt.h"

#include <stdio.h>
#include <string.h>

#include "rsg_internal.h"

RsgStr rsg_fmt_fixed(double value, int32_t decimals) {
    if (decimals < 0) {
        decimals = 0;
    }
    if (decimals > 20) {
        decimals = 20;
    }
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%.*f", decimals, value);
    return rsg_str_new(buf, (int32_t)len);
}

RsgStr rsg_fmt_hex(int64_t value) {
    char buf[32];
    int len;
    if (value < 0) {
        len = snprintf(buf, sizeof(buf), "-%llx", (unsigned long long)(-value));
    } else {
        len = snprintf(buf, sizeof(buf), "%llx", (unsigned long long)value);
    }
    return rsg_str_new(buf, (int32_t)len);
}

RsgStr rsg_fmt_bin(int64_t value) {
    if (value == 0) {
        return rsg_str_lit("0");
    }
    char buf[66]; // sign + 64 bits + NUL
    int pos = 65;
    buf[pos] = '\0';

    bool neg = value < 0;
    uint64_t v = neg ? (uint64_t)(-value) : (uint64_t)value;

    while (v > 0) {
        buf[--pos] = (char)('0' + (v & 1));
        v >>= 1;
    }
    if (neg) {
        buf[--pos] = '-';
    }
    int32_t len = (int32_t)(65 - pos);
    return rsg_str_new(buf + pos, len);
}

RsgStr rsg_fmt_oct(int64_t value) {
    char buf[32];
    int len;
    if (value < 0) {
        len = snprintf(buf, sizeof(buf), "-%llo", (unsigned long long)(-value));
    } else {
        len = snprintf(buf, sizeof(buf), "%llo", (unsigned long long)value);
    }
    return rsg_str_new(buf, (int32_t)len);
}

RsgStr rsg_fmt_pad_left(RsgStr s, int32_t width, char fill) {
    if (s.len >= width) {
        return s;
    }
    int32_t pad = width - s.len;
    char *buf = checked_malloc(width + 1);
    memset(buf, fill, pad);
    memcpy(buf + pad, s.data, s.len);
    buf[width] = '\0';
    return (RsgStr){.data = buf, .len = width, .ref_count = 1};
}

RsgStr rsg_fmt_pad_right(RsgStr s, int32_t width, char fill) {
    if (s.len >= width) {
        return s;
    }
    int32_t pad = width - s.len;
    char *buf = checked_malloc(width + 1);
    memcpy(buf, s.data, s.len);
    memset(buf + s.len, fill, pad);
    buf[width] = '\0';
    return (RsgStr){.data = buf, .len = width, .ref_count = 1};
}
