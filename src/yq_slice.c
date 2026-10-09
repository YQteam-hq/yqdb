#include "yq_slice.h"
#include <string.h>

void yq_slice_set(yq_slice *s, const void *data, size_t size) {
    s->data = data;
    s->size = size;
}

int yq_slice_equal(const yq_slice *a, const yq_slice *b) {
    if (a->size != b->size) return 0;
    if (a->size == 0) return 1;
    return memcmp(a->data, b->data, a->size) == 0;
}

int yq_slice_compare(const yq_slice *a, const yq_slice *b) {
    size_t min_len = a->size < b->size ? a->size : b->size;
    if (min_len > 0) {
        int cmp = memcmp(a->data, b->data, min_len);
        if (cmp != 0) return cmp < 0 ? -1 : 1;
    }
    if (a->size < b->size) return -1;
    if (a->size > b->size) return 1;
    return 0;
}
