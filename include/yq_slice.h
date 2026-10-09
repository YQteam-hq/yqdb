#ifndef YQ_SLICE_H
#define YQ_SLICE_H

#include <stddef.h>
#include <stdint.h>
#include "yq.h"

void yq_slice_set(yq_slice *s, const void *data, size_t size);
int yq_slice_equal(const yq_slice *a, const yq_slice *b);
int yq_slice_compare(const yq_slice *a, const yq_slice *b);

#endif
