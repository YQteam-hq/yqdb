#include <stdio.h>
#include <string.h>
#include "yq_test_check.h"
#include "yq_enc.h"
#include "yq_memblk.h"
#include "yq_memtable.h"

/*
 * A live memtable entry is billed key+value+slot, a tombstone only key+slot.
 * Reviving a deleted key must therefore end up billed exactly like a fresh
 * insert of the same key and value. When the tombstone cost was not reclaimed,
 * every delete/insert cycle left used_bytes permanently inflated, eventually
 * producing a spurious YQ_ERR_NOMEM for data the memtable never held.
 */
static void test_memtable_accounting(void) {
    yq_memtable *mt = yq_memtable_create(1u << 20);
    yq_memtable *ref = yq_memtable_create(1u << 20);
    CHECK(mt != NULL && ref != NULL);

    yq_slice k = {"alpha", 5};
    yq_slice v1 = {"v1", 2};
    yq_slice v2 = {"value-2", 7};

    /* delete-then-insert on the same key */
    CHECK(yq_memtable_put(mt, k, v1) == YQ_OK);
    CHECK(yq_memtable_del(mt, k) == YQ_OK);
    CHECK(yq_memtable_put(mt, k, v2) == YQ_OK);

    /* reference: the same key/value inserted once, into an empty memtable */
    CHECK(yq_memtable_put(ref, k, v2) == YQ_OK);
    CHECK(yq_memtable_bytes(mt) == yq_memtable_bytes(ref));

    /* and the revived entry actually holds the new value */
    yq_slice out = {0};
    CHECK(yq_memtable_get(mt, k, &out) == YQ_OK);
    CHECK(out.size == v2.size && memcmp(out.data, v2.data, v2.size) == 0);

    /* a plain overwrite must not change the accounting either */
    CHECK(yq_memtable_put(ref, k, v1) == YQ_OK);
    CHECK(yq_memtable_put(ref, k, v2) == YQ_OK);
    CHECK(yq_memtable_bytes(mt) == yq_memtable_bytes(ref));

    yq_memtable_destroy(mt);
    yq_memtable_destroy(ref);
}

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

    test_memtable_accounting();

    printf("basic tests passed\n");
    return 0;
}
