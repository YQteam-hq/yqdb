/*
 * Memtable regression tests.
 *
 * Pin the accounting fix in yq_memtable_put(): when a key is overwritten that
 * currently sits as a tombstone (delete-then-reinsert in the same memtable
 * instance), only the value length may change -- the key and the entry slot
 * are already charged to used_bytes. Charging the full entry_cost on that path
 * double-counted the key and the slot, so used_bytes grew without bound and
 * the memtable reported itself full long before it really was, spuriously
 * failing puts that still fit.
 *
 * The over-count is key_len + sizeof(mt_entry) per resurrection, which is tiny
 * compared with the real arena growth when values are large -- the bump
 * allocator's hard limit masks it. With an 8-byte value the over-count dominates
 * and the soft capacity cap trips after a few thousand cycles under the bug; the
 * fix keeps used_bytes flat and the whole run succeeds.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yq_test_check.h"
#include "yq_memtable.h"

int main(void) {
    printf("=== yq-DB Memtable Tests ===\n\n");

    /* 64 KiB soft cap; bump arena is 2x. With an 8-byte value the ~32-byte
     * per-cycle over-count reaches the soft cap after ~2k cycles, far sooner
     * than the arena's hard limit (~tens of thousands of cycles). */
    yq_memtable *mt = yq_memtable_create(64 * 1024);
    CHECK(mt != NULL);

    uint8_t key[16];
    memset(key, 'k', sizeof(key));
    uint8_t val[8];
    memset(val, 'v', sizeof(val));

    yq_slice k = { key, sizeof(key) };
    yq_slice v = { val, sizeof(val) };

    printf("test_tombstone_resurrection_accounting... ");
    for (int i = 0; i < 5000; i++) {
        CHECK(yq_memtable_put(mt, k, v) == YQ_OK);
        CHECK(yq_memtable_del(mt, k) == YQ_OK);   /* -> tombstone */
        CHECK(yq_memtable_put(mt, k, v) == YQ_OK); /* -> resurrect */
    }
    printf("OK\n");

    printf("test_final_state_consistent... ");
    yq_slice out = { NULL, 0 };
    CHECK(yq_memtable_get(mt, k, &out) == YQ_OK);
    CHECK(out.size == sizeof(val));
    CHECK(memcmp(out.data, val, sizeof(val)) == 0);
    CHECK(yq_memtable_del(mt, k) == YQ_OK);
    CHECK(yq_memtable_get(mt, k, &out) == YQ_ERR_NOTFOUND);
    printf("OK\n");

    yq_memtable_destroy(mt);

    printf("\n=== ALL TESTS PASSED ===\n");
    return 0;
}
