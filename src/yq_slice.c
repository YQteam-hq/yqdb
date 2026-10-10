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
    return yq_slice_compare_raw(a->data, a->size, b->data, b->size);
}
