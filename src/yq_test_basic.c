#include <stdio.h>
#include <string.h>
#include "yq_test_check.h"
#include "yq_enc.h"
#include "yq_memblk.h"
#include "yq_memtable.h"

int main(void) {
    uint8_t buf[64];
    size_t n;
    uint64_t val;
    size_t consumed;

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

    uint32_t crc = yq_crc32c("test", 4);
    CHECK(crc != 0);

    yq_memblk *blk = yq_memblk_create(4096);
    CHECK(blk != NULL);
    void *p = yq_memblk_alloc(blk, 16);
    CHECK(p != NULL);
    yq_memblk_reset(blk);
    p = yq_memblk_alloc(blk, 32);
    CHECK(p != NULL);
    yq_memblk_destroy(blk);

    /* Memtable accounting must stay exact across delete/put cycles.
     * put-over-tombstone used to add the full entry cost without
     * subtracting the tombstone it replaced, inflating used_bytes. */
    {
        yq_memtable *mt = yq_memtable_create(64 * 1024);
        CHECK(mt != NULL);
        yq_slice key, val;
        yq_slice_set(&key, "k", 1);
        static uint8_t vbuf[100];
        memset(vbuf, 'a', sizeof(vbuf));
        yq_slice_set(&val, vbuf, sizeof(vbuf));

        CHECK(yq_memtable_put(mt, key, val) == YQ_OK);
        size_t used_live = yq_memtable_bytes(mt);
        CHECK(yq_memtable_del(mt, key) == YQ_OK);
        CHECK(yq_memtable_bytes(mt) < used_live);
        CHECK(yq_memtable_put(mt, key, val) == YQ_OK);
        CHECK(yq_memtable_bytes(mt) == used_live);

        /* An in-place update swaps only the value bytes. */
        static uint8_t vbuf2[32];
        memset(vbuf2, 'b', sizeof(vbuf2));
        yq_slice val2;
        yq_slice_set(&val2, vbuf2, sizeof(vbuf2));
        CHECK(yq_memtable_put(mt, key, val2) == YQ_OK);
        CHECK(yq_memtable_bytes(mt) == used_live - sizeof(vbuf) + sizeof(vbuf2));

        yq_memtable_destroy(mt);
    }

    printf("basic tests passed\n");
    return 0;
}
