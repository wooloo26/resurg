#include "rsg_slice.h"

#include <string.h>

#include "rsg_gc.h"

RsgSlice rsg_slice_new(const void *src, int32_t count, size_t elem_size) {
    size_t total = (size_t)count * elem_size;
    void *data = rsg_heap_alloc(total);
    if (src != NULL && count > 0) {
        memcpy(data, src, total);
    }
    return (RsgSlice){.data = data, .len = count};
}

RsgSlice rsg_slice_sub(RsgSlice slice, int32_t start, int32_t end, size_t elem_size) {
    return (RsgSlice){
        .data = (char *)slice.data + (size_t)start * elem_size,
        .len = end - start,
    };
}

RsgSlice rsg_slice_from_array(const void *array_data, int32_t count, size_t elem_size) {
    return rsg_slice_new(array_data, count, elem_size);
}

RsgSlice rsg_slice_concat(RsgSlice a, RsgSlice b, size_t elem_size) {
    int32_t total = a.len + b.len;
    void *data = rsg_heap_alloc((size_t)total * elem_size);
    if (a.len > 0) {
        memcpy(data, a.data, (size_t)a.len * elem_size);
    }
    if (b.len > 0) {
        memcpy((char *)data + (size_t)a.len * elem_size, b.data, (size_t)b.len * elem_size);
    }
    return (RsgSlice){.data = data, .len = total};
}
