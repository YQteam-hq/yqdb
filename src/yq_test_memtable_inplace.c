/*
 * yq_test_memtable_inplace.c — repeated overwrites of one key must not consume
 * the memtable arena.
 *
 * yq_memtable_put() used to allocate fresh bytes for the value on every update
 * while the arena is a bump allocator that never reclaims, so the previous
 * value bytes were stranded. For an update-heavy workload (a hot key written
 * over and over) the arena grew without bound and eventually yq_memblk_alloc()
 * failed, making the PUT return YQ_ERR_NOMEM even though used_bytes -- the
 * logical accounting -- stayed tiny.
 *
 * With in-place overwrite, arena usage is bounded by the number of distinct
 * live keys, so the sequence below completes.
 */
#include "yq.h"
#include "yq_memtable.h"
#include "yq_test_check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    const size_t V = 1024;       /* value size */
    const size_t ROUNDS = 5000;  /* overwrites of the same key */

    /* 1 MiB logical memtable => 2 MiB arena. 5000 x 1 KiB of *leaked* value
     * bytes would need ~5 MiB, far past the arena. */
    yq_memtable *mt = yq_memtable_create(1024u * 1024u);
    CHECK(mt != NULL);

    uint8_t *val = (uint8_t *)malloc(V);
    CHECK(val != NULL);

    yq_slice k = { "hot-key", 7 };

    for (size_t i = 0; i < ROUNDS; i++) {
        memset(val, (int)(i & 0xFF), V);
        yq_slice v = { val, V };
        CHECK_EQ(yq_memtable_put(mt, k, v), YQ_OK);
    }

    /* The newest value is what a read must return. */
    yq_slice out = { 0 };
    CHECK_EQ(yq_memtable_get(mt, k, &out), YQ_OK);
    CHECK_EQ(out.size, V);
    memset(val, (int)((ROUNDS - 1) & 0xFF), V);
    CHECK(memcmp(out.data, val, V) == 0);

    /*
     * Accounting must stay bounded by a single live key's worth of bytes, not
     * by the number of writes -- that is the whole point of overwriting in
     * place. Before the fix the arena was exhausted (and on the failing PUT the
     * running total was left decremented, reading back as 0).
     */
    CHECK(yq_memtable_bytes(mt) >= V);
    CHECK(yq_memtable_bytes(mt) < 4 * V);

    /* A smaller value also fits the existing slot and updates in place. */
    yq_slice small = { val, 16 };
    CHECK_EQ(yq_memtable_put(mt, k, small), YQ_OK);
    out.data = NULL; out.size = 0;
    CHECK_EQ(yq_memtable_get(mt, k, &out), YQ_OK);
    CHECK_EQ(out.size, 16);

    /* A larger value than the slot still allocates and is readable. */
    {
        uint8_t *big = (uint8_t *)malloc(V * 4);
        CHECK(big != NULL);
        memset(big, 0x5A, V * 4);
        yq_slice bv = { big, V * 4 };
        CHECK_EQ(yq_memtable_put(mt, k, bv), YQ_OK);
        out.data = NULL; out.size = 0;
        CHECK_EQ(yq_memtable_get(mt, k, &out), YQ_OK);
        CHECK_EQ(out.size, V * 4);
        CHECK(memcmp(out.data, big, V * 4) == 0);
        free(big);
    }

    /* Distinct keys still allocate normally. */
    for (size_t i = 0; i < 100; i++) {
        char kb[32];
        snprintf(kb, sizeof(kb), "key-%zu", i);
        yq_slice kk = { kb, strlen(kb) };
        yq_slice vv = { val, 16 };
        CHECK_EQ(yq_memtable_put(mt, kk, vv), YQ_OK);
    }

    /* Delete then re-put: the tombstone owns no value region, so the re-put
     * allocates once and later updates reuse it. */
    CHECK_EQ(yq_memtable_del(mt, k), YQ_OK);
    out.data = NULL; out.size = 0;
    CHECK_EQ(yq_memtable_get(mt, k, &out), YQ_ERR_NOTFOUND);
    for (int i = 0; i < 500; i++) {
        yq_slice v = { val, 16 };
        CHECK_EQ(yq_memtable_put(mt, k, v), YQ_OK);
    }
    out.data = NULL; out.size = 0;
    CHECK_EQ(yq_memtable_get(mt, k, &out), YQ_OK);
    CHECK_EQ(out.size, 16);

    printf("memtable_inplace: OK (entries=%d used_bytes=%zu)\n",
           yq_memtable_size(mt), yq_memtable_bytes(mt));

    yq_memtable_destroy(mt);
    free(val);
    return 0;
}
