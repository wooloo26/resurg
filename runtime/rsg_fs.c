#include "rsg_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/types.h>
#endif

#include "rsg_gc.h"
#include "rsg_internal.h"
#include "rsg_slice.h"

// ── Helpers ───────────────────────────────────────────────────────────

/** Copy RsgStr to a NUL-terminated stack buffer for C API calls. */
static char *str_to_cstr(RsgStr s) {
    char *buf = checked_malloc(s.len + 1);
    memcpy(buf, s.data, s.len);
    buf[s.len] = '\0';
    return buf;
}

// ── File operations ───────────────────────────────────────────────────

RsgStr rsg_fs_read_file(RsgStr path) {
    char *cpath = str_to_cstr(path);
    FILE *f = fopen(cpath, "rb");
    free(cpath);
    if (f == NULL) {
        return rsg_str_empty();
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return rsg_str_empty();
    }

    char *buf = checked_malloc(size + 1);
    size_t nread = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[nread] = '\0';

    return (RsgStr){.data = buf, .len = (int32_t)nread, .ref_count = 1};
}

bool rsg_fs_write_file(RsgStr path, RsgStr content) {
    char *cpath = str_to_cstr(path);
    FILE *f = fopen(cpath, "wb");
    free(cpath);
    if (f == NULL) {
        return false;
    }
    if (content.len > 0) {
        size_t written = fwrite(content.data, 1, content.len, f);
        fclose(f);
        return written == (size_t)content.len;
    }
    fclose(f);
    return true;
}

bool rsg_fs_append_file(RsgStr path, RsgStr content) {
    char *cpath = str_to_cstr(path);
    FILE *f = fopen(cpath, "ab");
    free(cpath);
    if (f == NULL) {
        return false;
    }
    if (content.len > 0) {
        size_t written = fwrite(content.data, 1, content.len, f);
        fclose(f);
        return written == (size_t)content.len;
    }
    fclose(f);
    return true;
}

bool rsg_fs_exists(RsgStr path) {
    char *cpath = str_to_cstr(path);
    struct stat st;
    bool result = (stat(cpath, &st) == 0);
    free(cpath);
    return result;
}

bool rsg_fs_remove(RsgStr path) {
    char *cpath = str_to_cstr(path);
    int result = remove(cpath);
    free(cpath);
    return result == 0;
}

bool rsg_fs_rename(RsgStr old_path, RsgStr new_path) {
    char *cold = str_to_cstr(old_path);
    char *cnew = str_to_cstr(new_path);
    int result = rename(cold, cnew);
    free(cold);
    free(cnew);
    return result == 0;
}

bool rsg_fs_mkdir(RsgStr path) {
    char *cpath = str_to_cstr(path);
#ifdef _WIN32
    int result = _mkdir(cpath);
#else
    int result = mkdir(cpath, 0755);
#endif
    free(cpath);
    return result == 0;
}

RsgSlice rsg_fs_read_dir(RsgStr path) {
    RsgSlice empty = {NULL, 0};
    char *cpath = str_to_cstr(path);

#ifdef _WIN32
    // Build search pattern: "path\*"
    size_t plen = strlen(cpath);
    char *pattern = checked_malloc(plen + 3);
    memcpy(pattern, cpath, plen);
    pattern[plen] = '\\';
    pattern[plen + 1] = '*';
    pattern[plen + 2] = '\0';
    free(cpath);

    WIN32_FIND_DATAA fd;
    HANDLE hfind = FindFirstFileA(pattern, &fd);
    free(pattern);
    if (hfind == INVALID_HANDLE_VALUE) {
        return empty;
    }

    int32_t count = 0;
    int32_t capacity = 16;
    RsgStr *entries = checked_malloc(capacity * sizeof(RsgStr));
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) {
            continue;
        }
        if (count >= capacity) {
            capacity *= 2;
            entries = realloc(entries, capacity * sizeof(RsgStr));
        }
        entries[count++] = rsg_str_new(fd.cFileName, (int32_t)strlen(fd.cFileName));
    } while (FindNextFileA(hfind, &fd));
    FindClose(hfind);
#else
    DIR *d = opendir(cpath);
    free(cpath);
    if (d == NULL) {
        return empty;
    }

    int32_t count = 0;
    int32_t capacity = 16;
    RsgStr *entries = checked_malloc(capacity * sizeof(RsgStr));
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (count >= capacity) {
            capacity *= 2;
            entries = realloc(entries, capacity * sizeof(RsgStr));
        }
        entries[count++] = rsg_str_new(ent->d_name, (int32_t)strlen(ent->d_name));
    }
    closedir(d);
#endif

    RsgSlice result = rsg_slice_new(entries, count, sizeof(RsgStr));
    free(entries);
    return result;
}
