#include <stdio.h>
#include <assert.h>
#include "yq_enc.h"
#include "yq_memblk.h"

int main(void) {
    uint8_t buf[64];
    size_t n;
    uint64_t val;
    size_t consumed;

    int rc = yq_varint_encode(300, buf, &n);
    assert(rc == YQ_OK && n == 2);
    rc = yq_varint_decode(buf, n, &val, &consumed);
    assert(rc == YQ_OK && val == 300 && consumed == 2);

    yq_slice a, b;
    yq_slice_set(&a, "hello", 5);
    yq_slice_set(&b, "hello", 5);
    assert(yq_slice_equal(&a, &b) == 1);
    yq_slice_set(&b, "world", 5);
    assert(yq_slice_equal(&a, &b) == 0);
    assert(yq_slice_compare(&a, &b) < 0);

    uint32_t crc = yq_crc32c("test", 4);
    assert(crc != 0);

    yq_memblk *blk = yq_memblk_create(4096);
    assert(blk != NULL);
    void *p = yq_memblk_alloc(blk, 16);
    assert(p != NULL);
    yq_memblk_reset(blk);
    p = yq_memblk_alloc(blk, 32);
    assert(p != NULL);
    yq_memblk_destroy(blk);

    printf("basic tests passed\n");
    return 0;
}
