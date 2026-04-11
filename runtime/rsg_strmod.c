#include "rsg_strmod.h"

#include <stdio.h>
#include <string.h>

#include "rsg_internal.h"

RsgStr rsg_str_mod_join(RsgSlice parts, RsgStr sep) {
    if (parts.len == 0) {
        return rsg_str_empty();
    }

    const RsgStr *arr = (const RsgStr *)parts.data;
    if (parts.len == 1) {
        return arr[0];
    }

    // Calculate total length
    int32_t total = 0;
    for (int32_t i = 0; i < parts.len; i++) {
        total += arr[i].len;
    }
    total += sep.len * (parts.len - 1);

    char *buf = checked_malloc(total + 1);
    int32_t pos = 0;
    for (int32_t i = 0; i < parts.len; i++) {
        if (i > 0 && sep.len > 0) {
            memcpy(buf + pos, sep.data, sep.len);
            pos += sep.len;
        }
        memcpy(buf + pos, arr[i].data, arr[i].len);
        pos += arr[i].len;
    }
    buf[total] = '\0';
    return (RsgStr){.data = buf, .len = total, .ref_count = 1};
}

RsgStr rsg_str_mod_format_int(int64_t val, int32_t base) {
    char buf[66]; // sign + 64 binary digits + NUL
    int len;
    switch (base) {
    case 2: {
        if (val == 0) {
            return rsg_str_lit("0");
        }
        int pos = 65;
        buf[pos] = '\0';
        bool neg = val < 0;
        uint64_t v = neg ? (uint64_t)(-val) : (uint64_t)val;
        while (v > 0) {
            buf[--pos] = (char)('0' + (v & 1));
            v >>= 1;
        }
        if (neg) {
            buf[--pos] = '-';
        }
        len = 65 - pos;
        return rsg_str_new(buf + pos, (int32_t)len);
    }
    case 8:
        if (val < 0) {
            len = snprintf(buf, sizeof(buf), "-%llo", (unsigned long long)(-val));
        } else {
            len = snprintf(buf, sizeof(buf), "%llo", (unsigned long long)val);
        }
        return rsg_str_new(buf, (int32_t)len);
    case 16:
        if (val < 0) {
            len = snprintf(buf, sizeof(buf), "-%llx", (unsigned long long)(-val));
        } else {
            len = snprintf(buf, sizeof(buf), "%llx", (unsigned long long)val);
        }
        return rsg_str_new(buf, (int32_t)len);
    default:
        len = snprintf(buf, sizeof(buf), "%lld", (long long)val);
        return rsg_str_new(buf, (int32_t)len);
    }
}
