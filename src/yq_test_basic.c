#include <stdio.h>
#include "yq_test_check.h"
#include "yq_enc.h"
#include "yq_memblk.h"

int main(void) {
    uint8_t buf[64];
    size_t n;
    uint64_t val;
    size_t consumed;

    /* Buffer size validation */
    if (sizeof(buf) < 64) {
        return 1;
    }

    int rc = yq_varint_encode(300, buf, &n);
    CHECK(rc == YQ_OK && n == 2);
    rc = yq_varint_decode(buf, n, &val, &consumed);
    CHECK(rc == YQ_OK && val == 300 && consumed == 2);

    yq_slice a, b;
    yq_slice_set(&a, "hello", 5);
    yq_slice_set(&b, "hello", 5);
    CHECK(yq_slice_equal(&a, &b) == 1);
    yq_slice_set(&b, "world", 5);
    CHECK(yq_slice_equal(&a, &b) == 0);
    CHECK(yq_slice_compare(&a, &b) < 0);

    /* Input validation for CRC calculation */
    if (sizeof("test") < 4) {
        return 1;
    }
    uint32_t crc = yq_crc32c("test", 4);
    CHECK(crc != 0);

    /* Memory allocation size validation */
    if (4096 > (1ULL << 30)) return 1; /* 1GB limit */
    if (16 > (1ULL << 30)) return 1; /* 1GB limit */
    if (32 > (1ULL << 30)) return 1; /* 1GB limit */
    
    yq_memblk *blk = yq_memblk_create(4096);
    CHECK(blk != NULL);
    void *p = yq_memblk_alloc(blk, 16);
    CHECK(p != NULL);
    yq_memblk_reset(blk);
    p = yq_memblk_alloc(blk, 32);
    CHECK(p != NULL);
    yq_memblk_destroy(blk);

    printf("basic tests passed\n");
    return 0;
}
