#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "yq_test_check.h"
#include <time.h>
#include "yq.h"
#include "yq_mempool.h"

static const char *TEST_DB = "yqtest_integration.yqdb";

static void remove_db(void) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s.log", TEST_DB); remove(buf);
    snprintf(buf, sizeof(buf), "%s.shm", TEST_DB); remove(buf);
    snprintf(buf, sizeof(buf), "%s.lock", TEST_DB); remove(buf);
    remove(TEST_DB);
}

static double now_sec(void) {
    return (double)clock() / CLOCKS_PER_SEC;
}

static void test_open_close(void) {
    printf("test_open_close... ");
    remove_db();

    yq_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.flags = YQ_OPEN_CREATE;
    opts.page_size = 4096;

    yq_db *db = NULL;
    int rc = yq_open(TEST_DB, &opts, &db);
    CHECK_EQ(rc, YQ_OK);
    CHECK(db != NULL);

    rc = yq_close(db);
    CHECK_EQ(rc, YQ_OK);

    rc = yq_open(TEST_DB, &opts, &db);
    CHECK_EQ(rc, YQ_OK);

    rc = yq_close(db);
    CHECK_EQ(rc, YQ_OK);

    remove_db();
    printf("OK\n");
}

static void test_put_get(void) {
    printf("test_put_get... ");
    remove_db();

    yq_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.flags = YQ_OPEN_CREATE;

    yq_db *db = NULL;
    int rc = yq_open(TEST_DB, &opts, &db);
    CHECK_EQ(rc, YQ_OK);

    yq_txn *txn = NULL;
    rc = yq_txn_begin(db, YQ_TXN_READWRITE, &txn);
    CHECK_EQ(rc, YQ_OK);

    yq_slice key = {"k1", 2};
    yq_slice val = {"v1", 2};
    rc = yq_put(txn, key, val, 0);
    CHECK_EQ(rc, YQ_OK);

    yq_slice out = {0};
    rc = yq_get(txn, key, &out);
    CHECK_EQ(rc, YQ_OK);
    CHECK(out.size == 2);
    CHECK(memcmp(out.data, "v1", 2) == 0);

    rc = yq_txn_commit(txn);
    CHECK_EQ(rc, YQ_OK);

    rc = yq_close(db);
    CHECK_EQ(rc, YQ_OK);

    rc = yq_open(TEST_DB, &opts, &db);
    CHECK_EQ(rc, YQ_OK);

    rc = yq_txn_begin(db, YQ_TXN_READONLY, &txn);
    CHECK_EQ(rc, YQ_OK);

    rc = yq_get(txn, key, &out);
    CHECK_EQ(rc, YQ_OK);
    CHECK(out.size == 2);

    rc = yq_txn_commit(txn);
    CHECK_EQ(rc, YQ_OK);

    rc = yq_close(db);
    CHECK_EQ(rc, YQ_OK);

    remove_db();
    printf("OK\n");
}

static void test_delete(void) {
    printf("test_delete... ");
    remove_db();

    yq_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.flags = YQ_OPEN_CREATE;

    yq_db *db = NULL;
    yq_open(TEST_DB, &opts, &db);

    yq_txn *txn = NULL;
    yq_txn_begin(db, YQ_TXN_READWRITE, &txn);

    yq_slice k1 = {"k1", 2}, v1 = {"v1", 2};
    yq_put(txn, k1, v1, 0);

    yq_slice out = {0};
    int rc = yq_get(txn, k1, &out);
    CHECK_EQ(rc, YQ_OK);

    yq_del(txn, k1);
    rc = yq_get(txn, k1, &out);
    CHECK(rc == YQ_ERR_NOTFOUND);

    yq_txn_commit(txn);
    yq_close(db);

    remove_db();
    printf("OK\n");
}

static void test_cursor(void) {
    printf("test_cursor... ");
    remove_db();

    yq_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.flags = YQ_OPEN_CREATE;

    yq_db *db = NULL;
    yq_open(TEST_DB, &opts, &db);

    yq_txn *txn = NULL;
    yq_txn_begin(db, YQ_TXN_READWRITE, &txn);

    for (int i = 0; i < 20; i++) {
        char k[16], v[16];
        snprintf(k, sizeof(k), "key%02d", i);
        snprintf(v, sizeof(v), "val%02d", i);
        yq_slice key = {k, strlen(k)};
        yq_slice val = {v, strlen(v)};
        yq_put(txn, key, val, 0);
    }

    yq_txn_commit(txn);

    yq_txn_begin(db, YQ_TXN_READONLY, &txn);

    yq_cur *cur = NULL;
    int rc = yq_cur_open(txn, &cur);
    CHECK_EQ(rc, YQ_OK);

    int count = 0;
    for (rc = yq_cur_first(cur); rc == YQ_OK; rc = yq_cur_next(cur)) {
        yq_slice k = {0}, v = {0};
        yq_cur_key(cur, &k);
        yq_cur_val(cur, &v);
        CHECK(k.size > 0);
        CHECK(v.size > 0);
        count++;
    }
    CHECK(count == 20);

    yq_cur_close(cur);
    yq_txn_commit(txn);
    yq_close(db);

    remove_db();
    printf("OK\n");
}

static void test_txn_abort(void) {
    printf("test_txn_abort... ");
    remove_db();

    yq_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.flags = YQ_OPEN_CREATE;

    yq_db *db = NULL;
    yq_open(TEST_DB, &opts, &db);

    yq_txn *txn = NULL;
    yq_txn_begin(db, YQ_TXN_READWRITE, &txn);

    yq_slice k1 = {"abortkey", 8}, v1 = {"val", 3};
    yq_put(txn, k1, v1, 0);

    yq_txn_abort(txn);

    yq_txn_begin(db, YQ_TXN_READONLY, &txn);
    yq_slice out = {0};
    int rc = yq_get(txn, k1, &out);
    CHECK(rc == YQ_ERR_NOTFOUND);
    yq_txn_commit(txn);

    yq_close(db);
    remove_db();
    printf("OK\n");
}

static void test_nooverwrite(void) {
    printf("test_nooverwrite... ");
    remove_db();

    yq_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.flags = YQ_OPEN_CREATE;

    yq_db *db = NULL;
    yq_open(TEST_DB, &opts, &db);

    yq_txn *txn = NULL;
    yq_txn_begin(db, YQ_TXN_READWRITE, &txn);

    yq_slice k1 = {"key1", 4}, v1 = {"val1", 4}, v2 = {"val2", 4};
    yq_put(txn, k1, v1, YQ_PUT_NOOVERWRITE);
    int rc = yq_put(txn, k1, v2, YQ_PUT_NOOVERWRITE);
    CHECK(rc == YQ_ERR_EXISTS);

    yq_slice out = {0};
    yq_get(txn, k1, &out);
    CHECK(out.size == 4);
    CHECK(memcmp(out.data, "val1", 4) == 0);

    yq_txn_commit(txn);
    yq_close(db);
    remove_db();
    printf("OK\n");
}

static void test_checkpoint(void) {
    printf("test_checkpoint... ");
    remove_db();

    yq_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.flags = YQ_OPEN_CREATE;
    opts.memtable_bytes = 1024 * 1024;

    yq_db *db = NULL;
    yq_open(TEST_DB, &opts, &db);

    yq_txn *txn = NULL;
    yq_txn_begin(db, YQ_TXN_READWRITE, &txn);
    for (int i = 0; i < 100; i++) {
        char k[16], v[16];
        snprintf(k, sizeof(k), "k%03d", i);
        snprintf(v, sizeof(v), "v%03d", i);
        yq_slice key = {k, strlen(k)};
        yq_slice val = {v, strlen(v)};
        yq_put(txn, key, val, 0);
    }
    yq_txn_commit(txn);

    int rc = yq_checkpoint(db);
    CHECK_EQ(rc, YQ_OK);

    rc = yq_sync(db);
    CHECK_EQ(rc, YQ_OK);

    yq_txn_begin(db, YQ_TXN_READONLY, &txn);
    int count = 0;
    yq_cur *cur = NULL;
    yq_cur_open(txn, &cur);
    for (rc = yq_cur_first(cur); rc == YQ_OK; rc = yq_cur_next(cur)) {
        count++;
    }
    yq_cur_close(cur);
    yq_txn_commit(txn);
    CHECK(count == 100);

    yq_close(db);
    remove_db();
    printf("OK\n");
}

static void test_batch(void) {
    printf("test_batch (1000 keys)... ");
    remove_db();

    yq_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.flags = YQ_OPEN_CREATE;
    opts.memtable_bytes = 32 * 1024 * 1024;

    yq_db *db = NULL;
    yq_open(TEST_DB, &opts, &db);

    double t0 = now_sec();

    yq_txn *txn = NULL;
    yq_txn_begin(db, YQ_TXN_READWRITE, &txn);

    for (int i = 0; i < 1000; i++) {
        char k[32], v[64];
        snprintf(k, sizeof(k), "key%06d", i);
        snprintf(v, sizeof(v), "value%06d_for_test", i);
        yq_slice key = {k, strlen(k)};
        yq_slice val = {v, strlen(v)};
        yq_put(txn, key, val, 0);
    }

    int rc = yq_txn_commit(txn);
    CHECK_EQ(rc, YQ_OK);

    double t1 = now_sec();
    printf("(%.1f keys/sec) ", 1000.0 / (t1 - t0));

    yq_txn_begin(db, YQ_TXN_READONLY, &txn);
    int count = 0;
    for (int i = 0; i < 1000; i++) {
        char k[32];
        snprintf(k, sizeof(k), "key%06d", i);
        yq_slice key = {k, strlen(k)};
        yq_slice out = {0};
        if (yq_get(txn, key, &out) == YQ_OK) count++;
    }
    yq_txn_commit(txn);
    CHECK(count == 1000);

    double t2 = now_sec();
    printf("(%.1f gets/sec) ", 1000.0 / (t2 - t1));

    yq_close(db);
    remove_db();
    printf("OK\n");
}

static void test_batch_api(void) {
    printf("test_batch_api... ");
    remove_db();

    yq_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.flags = YQ_OPEN_CREATE;

    yq_db *db = NULL;
    int rc = yq_open(TEST_DB, &opts, &db);
    assert(rc == YQ_OK);

    /* Write a mixed batch of PUT and DELETE operations in one call. */
    yq_txn *txn = NULL;
    rc = yq_txn_begin(db, YQ_TXN_READWRITE, &txn);
    assert(rc == YQ_OK);

    char k0[] = "batch:k0", k1[] = "batch:k1", k2[] = "batch:k2";
    char k3[] = "batch:k3", k4[] = "batch:k4";
    char v1[] = "value-1", v2[] = "value-2", v3[] = "value-3";

    yq_batch_entry entries[5];
    memset(entries, 0, sizeof(entries));
    entries[0].key = (yq_slice){k0, strlen(k0)}; entries[0].val = (yq_slice){v1, strlen(v1)}; entries[0].op = 0;
    entries[1].key = (yq_slice){k1, strlen(k1)}; entries[1].val = (yq_slice){v2, strlen(v2)}; entries[1].op = 0;
    entries[2].key = (yq_slice){k2, strlen(k2)}; entries[2].val = (yq_slice){v3, strlen(v3)}; entries[2].op = 0;
    entries[3].key = (yq_slice){k3, strlen(k3)}; entries[3].op = 1; /* delete of a missing key is idempotent */
    entries[4].key = (yq_slice){k4, strlen(k4)}; entries[4].op = 1;

    yq_batch_result result;
    memset(&result, 0, sizeof(result));
    rc = yq_batch_put(txn, entries, 5, &result);
    assert(rc == YQ_OK);
    assert(result.struct_size == sizeof(yq_batch_result));
    assert(result.entries_total == 5);
    assert(result.entries_ok == 5);
    assert(result.entries_failed == 0);
    assert(yq_txn_commit(txn) == YQ_OK);

    /* Read several keys back through a single yq_batch_get() call. */
    rc = yq_txn_begin(db, YQ_TXN_READONLY, &txn);
    assert(rc == YQ_OK);

    yq_slice keys[4] = {
        {k0, strlen(k0)}, {k1, strlen(k1)}, {k3, strlen(k3)}, {k2, strlen(k2)}
    };
    yq_slice vals[4];
    size_t found = 0;
    rc = yq_batch_get(txn, keys, 4, vals, &found);
    assert(rc == YQ_OK);
    assert(found == 3);
    assert(vals[0].size == strlen(v1) && memcmp(vals[0].data, v1, vals[0].size) == 0);
    assert(vals[1].size == strlen(v2) && memcmp(vals[1].data, v2, vals[1].size) == 0);
    assert(vals[2].data == NULL && vals[2].size == 0); /* k3 was never written */
    assert(vals[3].size == strlen(v3) && memcmp(vals[3].data, v3, vals[3].size) == 0);

    /* A read-only transaction must reject batch writes. */
    yq_batch_result ro;
    memset(&ro, 0, sizeof(ro));
    rc = yq_batch_put(txn, entries, 1, &ro);
    assert(rc == YQ_ERR_READONLY);

    /* The same read-only guard must cover single-key writes. */
    rc = yq_put(txn, entries[0].key, entries[0].val, 0);
    assert(rc == YQ_ERR_READONLY);
    yq_txn_commit(txn);

    /* A malformed entry rejects the whole batch before any mutation, and the
     * counters stay self-consistent: total == ok + failed. */
    rc = yq_txn_begin(db, YQ_TXN_READWRITE, &txn);
    assert(rc == YQ_OK);
    yq_batch_entry bad;
    memset(&bad, 0, sizeof(bad)); /* key.data == NULL */
    yq_batch_result bad_result;
    memset(&bad_result, 0, sizeof(bad_result));
    rc = yq_batch_put(txn, &bad, 1, &bad_result);
    assert(rc == YQ_ERR_INVAL);
    assert(bad_result.entries_total == 1);
    assert(bad_result.entries_ok == 0);
    assert(bad_result.entries_failed == 1);
    assert(bad_result.entries_ok + bad_result.entries_failed == bad_result.entries_total);

    /* A batch of 5 where one entry is malformed must not report the other 4
     * as ok: everything is counted as failed because nothing was applied. */
    {
        char bk[5][16] = {"bx0", "bx1", "bx2", "bx3", "bx4"};
        char bv[5][16] = {"v0", "v1", "v2", "v3", "v4"};
        yq_batch_entry batch5[5];
        for (int i = 0; i < 5; i++) {
            batch5[i].key.data = bk[i];
            batch5[i].key.size = strlen(bk[i]);
            batch5[i].val.data = bv[i];
            batch5[i].val.size = strlen(bv[i]);
            batch5[i].op = 0;
            batch5[i].flags = 0;
        }
        /* entry 3 is malformed */
        batch5[3].key.size = 0;

        yq_batch_result r5;
        memset(&r5, 0, sizeof(r5));
        rc = yq_batch_put(txn, batch5, 5, &r5);
        assert(rc == YQ_ERR_INVAL);
        assert(r5.entries_total == 5);
        assert(r5.entries_ok == 0);
        assert(r5.entries_failed == 5);
        assert(r5.entries_ok + r5.entries_failed == r5.entries_total);
        assert(r5.first_error == YQ_ERR_INVAL);

        /* and nothing was applied: the valid entries must not be visible */
        yq_slice probe = {0};
        yq_slice k0s = {bk[0], strlen(bk[0])};
        assert(yq_get(txn, k0s, &probe) == YQ_ERR_NOTFOUND);
    }

    /* result is optional — NULL must be accepted, not rejected as INVAL. */
    {
        char nk[8] = "nullres";
        char nv[8] = "v";
        yq_batch_entry ne[1];
        ne[0].key.data = nk;
        ne[0].key.size = strlen(nk);
        ne[0].val.data = nv;
        ne[0].val.size = strlen(nv);
        ne[0].op = 0;
        ne[0].flags = 0;
        rc = yq_batch_put(txn, ne, 1, NULL);
        assert(rc == YQ_OK);

        yq_slice qk = {nk, strlen(nk)};
        yq_slice qv = {0};
        assert(yq_get(txn, qk, &qv) == YQ_OK);
        assert(qv.size == 1 && memcmp(qv.data, "v", 1) == 0);

        /* batch_del with NULL result too */
        rc = yq_batch_del(txn, &qk, 1, NULL);
        assert(rc == YQ_OK);
        assert(yq_get(txn, qk, &qv) == YQ_ERR_NOTFOUND);
    }

    /* succeed/fail counting on the apply path also stays consistent */
    {
        char ck[3][16] = {"ck0", "ck1", "ck2"};
        char cv[3][16] = {"a", "b", "c"};
        yq_batch_entry ce[3];
        for (int i = 0; i < 3; i++) {
            ce[i].key.data = ck[i];
            ce[i].key.size = strlen(ck[i]);
            ce[i].val.data = cv[i];
            ce[i].val.size = strlen(cv[i]);
            ce[i].op = 0;
            ce[i].flags = 0;
        }
        yq_batch_result cr;
        memset(&cr, 0, sizeof(cr));
        rc = yq_batch_put(txn, ce, 3, &cr);
        assert(rc == YQ_OK);
        assert(cr.entries_total == 3);
        assert(cr.entries_ok == 3);
        assert(cr.entries_failed == 0);
        assert(cr.first_error == YQ_OK);
        assert(cr.entries_ok + cr.entries_failed == cr.entries_total);
    }
    yq_txn_abort(txn);

    /* yq_batch_del removes existing keys. */
    rc = yq_txn_begin(db, YQ_TXN_READWRITE, &txn);
    assert(rc == YQ_OK);
    yq_slice del_keys[2] = {{k0, strlen(k0)}, {k1, strlen(k1)}};
    yq_batch_result del_result;
    memset(&del_result, 0, sizeof(del_result));
    rc = yq_batch_del(txn, del_keys, 2, &del_result);
    assert(rc == YQ_OK);
    assert(del_result.entries_ok == 2);
    assert(yq_txn_commit(txn) == YQ_OK);

    rc = yq_txn_begin(db, YQ_TXN_READONLY, &txn);
    assert(rc == YQ_OK);
    yq_slice out = {0};
    assert(yq_get(txn, del_keys[0], &out) == YQ_ERR_NOTFOUND);
    yq_txn_commit(txn);

    /* The small-object memory pool is functional. */
    yq_mempool *pool = yq_mempool_create();
    assert(pool != NULL);
    void *mpp = yq_mempool_alloc(pool, 64);
    assert(mpp != NULL);
    yq_mempool_free(pool, mpp);

    /* stats uses the project-wide YQ_ERR_* convention, not a bare -1 */
    yq_mempool_stats mps;
    memset(&mps, 0, sizeof(mps));
    mps.struct_size = sizeof(mps);
    assert(yq_mempool_stats_get(pool, &mps) == YQ_OK);
    assert(yq_mempool_stats_get(NULL, &mps) == YQ_ERR_INVAL);
    assert(yq_mempool_stats_get(pool, NULL) == YQ_ERR_INVAL);
    mps.struct_size = 0;
    assert(yq_mempool_stats_get(pool, &mps) == YQ_ERR_INVAL);

    yq_mempool_destroy(pool);

    yq_close(db);
    remove_db();
    printf("OK\n");
}

static void test_concurrent_readers(void) {
    printf("test_concurrent_readers... ");
    remove_db();

    yq_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.flags = YQ_OPEN_CREATE;
    opts.max_readers = 126;

    yq_db *db = NULL;
    yq_open(TEST_DB, &opts, &db);

    yq_txn *txn = NULL;
    yq_txn_begin(db, YQ_TXN_READWRITE, &txn);
    for (int i = 0; i < 50; i++) {
        char k[16], v[16];
        snprintf(k, sizeof(k), "key%02d", i);
        snprintf(v, sizeof(v), "val%02d", i);
        yq_slice key = {k, strlen(k)};
        yq_slice val = {v, strlen(v)};
        yq_put(txn, key, val, 0);
    }
    yq_txn_commit(txn);

    int total_got = 0;
    for (int r = 0; r < 5; r++) {
        yq_txn *rtxn = NULL;
        int rc = yq_txn_begin(db, YQ_TXN_READONLY, &rtxn);
        if (rc == YQ_OK) {
            for (int i = 0; i < 50; i++) {
                char k[16];
                snprintf(k, sizeof(k), "key%02d", i);
                yq_slice key = {k, strlen(k)};
                yq_slice out = {0};
                if (yq_get(rtxn, key, &out) == YQ_OK) total_got++;
            }
            yq_txn_commit(rtxn);
        }
    }
    CHECK(total_got == 250);

    yq_close(db);
    remove_db();
    printf("OK\n");
}

static void test_stat(void) {
    printf("test_stat... ");
    remove_db();

    yq_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.flags = YQ_OPEN_CREATE;

    yq_db *db = NULL;
    yq_open(TEST_DB, &opts, &db);

    yq_stat st;
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);

    int rc = yq_db_stat(db, &st);
    CHECK_EQ(rc, YQ_OK);
    CHECK(st.struct_size == sizeof(yq_stat));
    CHECK(st.format_version == 1);
    CHECK(st.page_size == 4096);

    yq_close(db);
    remove_db();
    printf("OK\n");
}

static void test_nosync(void) {
    printf("test_nosync... ");
    remove_db();

    yq_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.flags = YQ_OPEN_CREATE | YQ_OPEN_NOSYNC;

    yq_db *db = NULL;
    int rc = yq_open(TEST_DB, &opts, &db);
    assert(rc == YQ_OK);
    assert(db != NULL);

    yq_txn *txn = NULL;
    rc = yq_txn_begin(db, YQ_TXN_READWRITE, &txn);
    assert(rc == YQ_OK);

    for (int i = 0; i < 10; i++) {
        char k[16], v[16];
        snprintf(k, sizeof(k), "k%02d", i);
        snprintf(v, sizeof(v), "v%02d", i);
        yq_slice key = {k, strlen(k)};
        yq_slice val = {v, strlen(v)};
        rc = yq_put(txn, key, val, 0);
        assert(rc == YQ_OK);
    }
    rc = yq_txn_commit(txn);
    assert(rc == YQ_OK);

    rc = yq_txn_begin(db, YQ_TXN_READONLY, &txn);
    assert(rc == YQ_OK);
    int count = 0;
    for (int i = 0; i < 10; i++) {
        char k[16];
        snprintf(k, sizeof(k), "k%02d", i);
        yq_slice key = {k, strlen(k)};
        yq_slice out = {0};
        if (yq_get(txn, key, &out) == YQ_OK && out.size > 0) count++;
    }
    assert(count == 10);

    rc = yq_txn_commit(txn);
    assert(rc == YQ_OK);
    rc = yq_close(db);
    assert(rc == YQ_OK);

    remove_db();
    printf("OK\n");
}

static void test_strerror(void) {
    printf("test_strerror... ");
    CHECK(strcmp(yq_strerror(YQ_OK), "success") == 0);
    CHECK(strcmp(yq_strerror(YQ_ERR_NOMEM), "out of memory") == 0);
    CHECK(strcmp(yq_strerror(YQ_ERR_NOTFOUND), "key not found") == 0);
    CHECK(strcmp(yq_strerror(99), "unknown error") == 0);
    printf("OK\n");
}

static void test_version(void) {
    printf("test_version... ");
    int major = -1, minor = -1, patch = -1;
    int rc = yq_version(&major, &minor, &patch);
    CHECK_EQ(rc, YQ_OK);
    CHECK(major == 1);
    CHECK(minor == 0);
    CHECK(patch == 0);
    printf("OK\n");
}

/*
 * Reader-slot acquisition / release.
 *
 * Regression guard for the shared-memory slot CAS: when the claim never
 * reported success, every read-write yq_txn_begin() returned
 * YQ_ERR_READER_FULL on POSIX while still marking every slot active.
 */
static void test_reader_slots(void) {
    printf("test_reader_slots... ");
    remove_db();

    yq_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.flags = YQ_OPEN_CREATE;
    opts.page_size = 4096;
    opts.max_readers = 2;

    yq_db *db = NULL;
    int rc = yq_open(TEST_DB, &opts, &db);
    CHECK_EQ(rc, YQ_OK);

    yq_stat st;
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK(yq_db_stat(db, &st) == YQ_OK);
    CHECK(st.max_readers == 2);
    CHECK(st.active_readers == 0);

    /* A read-write transaction must be able to claim a slot and write. */
    yq_txn *w = NULL;
    CHECK(yq_txn_begin(db, YQ_TXN_READWRITE, &w) == YQ_OK);
    CHECK(yq_db_stat(db, &st) == YQ_OK);
    CHECK(st.active_readers == 1);

    yq_slice k = {"rk", 2};
    yq_slice v = {"rv", 2};
    CHECK(yq_put(w, k, v, YQ_PUT_UPSERT) == YQ_OK);
    CHECK(yq_txn_commit(w) == YQ_OK);

    /* Committing releases the slot, so the next transaction can claim one. */
    CHECK(yq_db_stat(db, &st) == YQ_OK);
    CHECK(st.active_readers == 0);

    CHECK(yq_txn_begin(db, YQ_TXN_READWRITE, &w) == YQ_OK);
    CHECK(yq_txn_abort(w) == YQ_OK);
    CHECK(yq_db_stat(db, &st) == YQ_OK);
    CHECK(st.active_readers == 0);

    CHECK(yq_close(db) == YQ_OK);
    remove_db();
    printf("OK\n");
}

/*
 * Read-only transactions must still register an MVCC reader slot.
 *
 * Regression guard for the YQ_TXN_READONLY flag test: the constant is 0, so
 * `flags & YQ_TXN_READONLY` is always false. Written that way, a read-only
 * transaction took neither branch in yq_txn_begin() and ended up with no
 * snapshot at all — invisible to yq_mvcc_reclaim_watermark().
 */
static void test_readonly_snapshot(void) {
    printf("test_readonly_snapshot... ");
    remove_db();

    yq_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.flags = YQ_OPEN_CREATE;
    opts.page_size = 4096;
    opts.max_readers = 2;

    yq_db *db = NULL;
    CHECK(yq_open(TEST_DB, &opts, &db) == YQ_OK);

    /* Seed one key with a write transaction. */
    yq_txn *w = NULL;
    CHECK(yq_txn_begin(db, YQ_TXN_READWRITE, &w) == YQ_OK);
    yq_slice k = {"rk", 2};
    yq_slice v = {"rv", 2};
    CHECK(yq_put(w, k, v, YQ_PUT_UPSERT) == YQ_OK);
    CHECK(yq_txn_commit(w) == YQ_OK);

    yq_stat st;
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);

    /* Each read-only transaction must claim exactly one reader slot. */
    yq_txn *a = NULL, *b = NULL, *c = NULL;
    CHECK(yq_txn_begin(db, YQ_TXN_READONLY, &a) == YQ_OK);
    CHECK(yq_db_stat(db, &st) == YQ_OK);
    CHECK(st.active_readers == 1);

    CHECK(yq_txn_begin(db, YQ_TXN_READONLY, &b) == YQ_OK);
    CHECK(yq_db_stat(db, &st) == YQ_OK);
    CHECK(st.active_readers == 2);

    /* Slots exhausted: the next reader is rejected rather than going
     * unregistered, which is what makes the reclaim watermark correct. */
    int rc = yq_txn_begin(db, YQ_TXN_READONLY, &c);
    CHECK(rc == YQ_ERR_READER_FULL);
    CHECK(c == NULL);

    /* A read-only snapshot sees committed data, and cannot write. */
    yq_slice out = {0};
    CHECK(yq_get(a, k, &out) == YQ_OK);
    CHECK(out.size == 2 && memcmp(out.data, "rv", 2) == 0);
    CHECK(yq_put(a, k, v, YQ_PUT_UPSERT) == YQ_ERR_READONLY);
    CHECK(yq_del(a, k) == YQ_ERR_READONLY);

    CHECK(yq_txn_commit(a) == YQ_OK);
    CHECK(yq_db_stat(db, &st) == YQ_OK);
    CHECK(st.active_readers == 1);

    CHECK(yq_txn_begin(db, YQ_TXN_READONLY, &c) == YQ_OK);
    CHECK(yq_txn_commit(b) == YQ_OK);
    CHECK(yq_txn_abort(c) == YQ_OK);
    CHECK(yq_db_stat(db, &st) == YQ_OK);
    CHECK(st.active_readers == 0);

    CHECK(yq_close(db) == YQ_OK);
    remove_db();
    printf("OK\n");
}

int main(void) {
    printf("=== yq-DB Integration Tests ===\n\n");

    test_version();
    test_strerror();
    test_open_close();
    test_put_get();
    test_delete();
    test_nooverwrite();
    test_txn_abort();
    test_cursor();
    test_checkpoint();
    test_concurrent_readers();
    test_batch();
    test_batch_api();
    test_stat();
    test_reader_slots();
    test_readonly_snapshot();
    test_nosync();

    printf("\n=== ALL TESTS PASSED ===\n");
    return 0;
}
