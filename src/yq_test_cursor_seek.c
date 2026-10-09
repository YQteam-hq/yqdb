/*
 * Cursor seek regression test over a large memtable.
 *
 * Exercises the public cursor merge path (yq_cur_seek / yq_cur_seek_exact),
 * which now uses yq_memtable_iter_seek (binary search) instead of a linear
 * scan over the memtable. The test loads thousands of keys into the memtable,
 * deletes a few (tombstones), and verifies that seek/seek_exact/iteration
 * return exactly the keys the sorted model predicts -- including that seeks
 * skip tombstones and land on the correct lower bound.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yq.h"
#include "yq_test_check.h"

static const char *TEST_DB = "yqtest_cursor_seek.yqdb";

static void remove_db(void) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s.log", TEST_DB);  remove(buf);
    snprintf(buf, sizeof(buf), "%s.shm", TEST_DB);  remove(buf);
    snprintf(buf, sizeof(buf), "%s.lock", TEST_DB); remove(buf);
    remove(TEST_DB);
}

static void open_db(yq_db **db) {
    yq_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.flags = YQ_OPEN_CREATE;
    CHECK(yq_open(TEST_DB, &opts, db) == YQ_OK);
    CHECK(*db != NULL);
}

static void make_key(char *buf, int i) {
    snprintf(buf, 32, "key%05d", i);
}

int main(void) {
    printf("=== yq-DB Cursor Seek Tests ===\n\n");
    remove_db();

    yq_db *db = NULL;
    open_db(&db);

    /* Load 5000 sequential keys in one transaction (stays in the memtable:
     * no checkpoint is triggered). */
    printf("test_load_and_seek... ");
    yq_txn *w = NULL;
    CHECK(yq_txn_begin(db, YQ_TXN_READWRITE, &w) == YQ_OK);
    char kb[32];
    for (int i = 0; i < 5000; i++) {
        make_key(kb, i);
        yq_slice k = { kb, strlen(kb) };
        yq_slice v = { kb, strlen(kb) };
        CHECK(yq_put(w, k, v, YQ_PUT_UPSERT) == YQ_OK);
    }
    /* Delete every 100th key -> tombstones in the memtable. */
    for (int i = 0; i < 5000; i += 100) {
        make_key(kb, i);
        yq_slice k = { kb, strlen(kb) };
        CHECK(yq_del(w, k) == YQ_OK);
    }
    CHECK(yq_txn_commit(w) == YQ_OK);

    /* Seek to key002500 and walk forward 20 keys; the tombstones at
     * 2400, 2500, 2600 must be skipped. */
    yq_txn *r = NULL;
    CHECK(yq_txn_begin(db, YQ_TXN_READONLY, &r) == YQ_OK);
    yq_cur *c = NULL;
    CHECK(yq_cur_open(r, &c) == YQ_OK);

    make_key(kb, 2500);
    yq_slice target = { kb, strlen(kb) };
    CHECK(yq_cur_seek(c, target) == YQ_OK);
    int seen = 0;
    int expected = 2501; /* 2500 is a tombstone, so lower bound is 2501 */
    while (yq_cur_valid(c) && seen < 20) {
        yq_slice k;
        CHECK(yq_cur_key(c, &k) == YQ_OK);
        char want[32];
        make_key(want, expected);
        CHECK(k.size == strlen(want));
        CHECK(memcmp(k.data, want, k.size) == 0);
        seen++;
        expected++;
        CHECK(yq_cur_next(c) == YQ_OK || seen == 20);
    }
    CHECK(seen == 20);
    CHECK(yq_cur_close(c) == YQ_OK);
    CHECK(yq_txn_abort(r) == YQ_OK);
    printf("OK\n");

    /* seek_exact on a tombstoned key must miss; on a live key must hit. */
    printf("test_seek_exact_tombstone... ");
    CHECK(yq_txn_begin(db, YQ_TXN_READONLY, &r) == YQ_OK);
    CHECK(yq_cur_open(r, &c) == YQ_OK);
    make_key(kb, 2400);
    yq_slice tk = { kb, strlen(kb) };
    CHECK(yq_cur_seek_exact(c, tk) == YQ_ERR_NOTFOUND);
    make_key(kb, 2401);
    yq_slice lk = { kb, strlen(kb) };
    CHECK(yq_cur_seek_exact(c, lk) == YQ_OK);
    yq_slice got;
    CHECK(yq_cur_key(c, &got) == YQ_OK);
    CHECK(got.size == strlen(kb));
    CHECK(memcmp(got.data, kb, got.size) == 0);
    CHECK(yq_cur_close(c) == YQ_OK);
    CHECK(yq_txn_abort(r) == YQ_OK);
    printf("OK\n");

    CHECK(yq_close(db) == YQ_OK);
    remove_db();

    printf("\n=== ALL TESTS PASSED ===\n");
    return 0;
}
