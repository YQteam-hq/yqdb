#ifndef YQ_SLICE_H
#define YQ_SLICE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "yq.h"

void yq_slice_set(yq_slice *s, const void *data, size_t size);
int yq_slice_equal(const yq_slice *a, const yq_slice *b);
int yq_slice_compare(const yq_slice *a, const yq_slice *b);

/*
 * Compare two raw key spans, returning <0, 0 or >0.
 *
 * Same contract as yq_slice_compare(), but taking pointers and lengths
 * directly so it can be inlined into hot search loops. A binary search over
 * the memtable or a B+Tree page runs this on every probe; going through
 * yq_slice_compare() instead costs an out-of-line call plus the construction
 * of two temporary yq_slice values per comparison.
 */
static inline int yq_slice_compare_raw(const void *a, size_t a_len,
                                       const void *b, size_t b_len) {
    size_t min_len = a_len < b_len ? a_len : b_len;
    if (min_len > 0) {
        int cmp = memcmp(a, b, min_len);
        if (cmp != 0) return cmp < 0 ? -1 : 1;
    }
    if (a_len < b_len) return -1;
    if (a_len > b_len) return 1;
    return 0;
}

#endif
