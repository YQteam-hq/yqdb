#ifndef YQ_ENC_H
#define YQ_ENC_H

#include <stddef.h>
#include <stdint.h>
#include "yq.h"

uint32_t yq_crc32c(const void *data, size_t len);
int yq_varint_encode(uint64_t val, uint8_t *out, size_t *nout);
int yq_varint_decode(const uint8_t *in, size_t inlen, uint64_t *out, size_t *nconsumed);
void yq_slice_set(yq_slice *s, const void *data, size_t size);
int yq_slice_equal(const yq_slice *a, const yq_slice *b);
int yq_slice_compare(const yq_slice *a, const yq_slice *b);

#endif
