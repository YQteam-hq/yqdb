#include "yq_slice.h"
#include <string.h>

#define YQ_SLICE_MAX_SIZE (1ULL << 30) /* 1GB limit */

void yq_slice_set(yq_slice *s, const void *data, size_t size) {
    /* Validate input parameters */
    if (!s) return;
    
    /* Validate size bounds */
    if (size > YQ_SLICE_MAX_SIZE) {
        size = 0; /* Set to empty slice if size is too large */
        data = NULL;
    }
    
    s->data = data;
    s->size = size;
}

int yq_slice_equal(const yq_slice *a, const yq_slice *b) {
    /* Validate input parameters */
    if (!a || !b) return 0;
    
    /* Validate size bounds */
    if (a->size > YQ_SLICE_MAX_SIZE || b->size > YQ_SLICE_MAX_SIZE) {
        return 0;
    }
    
    if (a->size != b->size) return 0;
    if (a->size == 0) return 1;
    
    /* Validate data pointers before comparing */
    if (!a->data || !b->data) {
        return a->data == b->data; /* Both NULL or both non-NULL */
    }
    
    return memcmp(a->data, b->data, a->size) == 0;
}

int yq_slice_compare(const yq_slice *a, const yq_slice *b) {
    /* Validate input parameters */
    if (!a || !b) return 0;
    
    /* Validate size bounds */
    if (a->size > YQ_SLICE_MAX_SIZE || b->size > YQ_SLICE_MAX_SIZE) {
        return 0;
    }
    
    size_t min_len = a->size < b->size ? a->size : b->size;
    
    /* Validate data pointers before comparing */
    if (min_len > 0) {
        if (!a->data || !b->data) {
            return a->data == b->data ? 0 : (!a->data ? -1 : 1);
        }
        
        int cmp = memcmp(a->data, b->data, min_len);
        if (cmp != 0) return cmp < 0 ? -1 : 1;
    }
    
    if (a->size < b->size) return -1;
    if (a->size > b->size) return 1;
    return 0;
}
