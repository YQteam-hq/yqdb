/*
 * _GNU_SOURCE must be defined before any libc header for mallinfo2().
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include "yq_test_check.h"
#include <time.h>
#include "yq.h"
#include "yq_mempool.h"

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

/*
 * mallinfo2() is glibc >= 2.33. When it is available the suite can assert
 * that yq_close() really hands memory back, instead of only looking for
 * wrong values.
 */
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
#define YQ_TEST_CAN_MEASURE_HEAP 1
#endif

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

/*
 * Checkpoint must compact the log down to the live dataset, without losing
 * anything: overwritten values and deleted keys are dropped, live keys
 * survive a close/reopen cycle.
 */
static void test_checkpoint_compaction(void) {
    printf("test_checkpoint_compaction... ");
    remove_db();

    yq_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.flags = YQ_OPEN_CREATE;
    opts.page_size = 4096;

    yq_db *db = NULL;
    CHECK_EQ(yq_open(TEST_DB, &opts, &db), YQ_OK);

    /* One key rewritten many times: the log grows, the live set does not. */
    for (int i = 0; i < 100; i++) {
        char v[32];
        snprintf(v, sizeof(v), "value-%d", i);
        yq_txn *t = NULL;
        CHECK_EQ(yq_txn_begin(db, YQ_TXN_READWRITE, &t), YQ_OK);
        yq_slice k = {"hot", 3};
        yq_slice vs = {v, strlen(v)};
        CHECK_EQ(yq_put(t, k, vs, YQ_PUT_UPSERT), YQ_OK);
        CHECK_EQ(yq_txn_commit(t), YQ_OK);
    }

    /* Keys that are written and then deleted must not come back. */
    for (int i = 0; i < 50; i++) {
        char k[32];
        snprintf(k, sizeof(k), "gone%03d", i);
        yq_txn *t = NULL;
        CHECK_EQ(yq_txn_begin(db, YQ_TXN_READWRITE, &t), YQ_OK);
        yq_slice ks = {k, strlen(k)};
        yq_slice vs = {"x", 1};
        CHECK_EQ(yq_put(t, ks, vs, YQ_PUT_UPSERT), YQ_OK);
        CHECK_EQ(yq_txn_commit(t), YQ_OK);

        t = NULL;
        CHECK_EQ(yq_txn_begin(db, YQ_TXN_READWRITE, &t), YQ_OK);
        CHECK_EQ(yq_del(t, ks), YQ_OK);
        CHECK_EQ(yq_txn_commit(t), YQ_OK);
    }

    yq_txn *t = NULL;
    CHECK_EQ(yq_txn_begin(db, YQ_TXN_READWRITE, &t), YQ_OK);
    yq_slice keep_k = {"keep", 4};
    yq_slice keep_v = {"yes", 3};
    CHECK_EQ(yq_put(t, keep_k, keep_v, YQ_PUT_UPSERT), YQ_OK);
    CHECK_EQ(yq_txn_commit(t), YQ_OK);

    yq_stat st;
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_EQ(yq_db_stat(db, &st), YQ_OK);
    uint64_t before = st.log_bytes;
    CHECK(before > 4096);

    CHECK_EQ(yq_checkpoint(db), YQ_OK);
    CHECK_EQ(yq_db_stat(db, &st), YQ_OK);
    CHECK(st.log_bytes < before / 2);

    CHECK_EQ(yq_close(db), YQ_OK);

    /* Everything live must still be there after a full reopen. */
    db = NULL;
    CHECK_EQ(yq_open(TEST_DB, &opts, &db), YQ_OK);
    t = NULL;
    CHECK_EQ(yq_txn_begin(db, YQ_TXN_READONLY, &t), YQ_OK);

    yq_slice out = {0};
    CHECK_EQ(yq_get(t, keep_k, &out), YQ_OK);
    CHECK(out.size == 3 && memcmp(out.data, "yes", 3) == 0);

    yq_slice hot = {"hot", 3};
    CHECK_EQ(yq_get(t, hot, &out), YQ_OK);
    CHECK(out.size == 8 && memcmp(out.data, "value-99", 8) == 0);

    for (int i = 0; i < 50; i++) {
        char k[32];
        snprintf(k, sizeof(k), "gone%03d", i);
        yq_slice ks = {k, strlen(k)};
        CHECK_EQ(yq_get(t, ks, &out), YQ_ERR_NOTFOUND);
    }

    CHECK_EQ(yq_txn_commit(t), YQ_OK);
    CHECK_EQ(yq_close(db), YQ_OK);
    remove_db();
    printf("OK\n");
}

/*
 * Write-lock contention must honour lock_timeout_ms.
 *
 * Regression guard for the writer election: it used a blocking flock(), so a
 * second writer hung in the kernel forever instead of returning YQ_ERR_BUSY
 * (lock_timeout_ms == 0) or YQ_ERR_TIMEOUT (budget exhausted).
 */
static void test_writer_lock_contention(void) {
    printf("test_writer_lock_contention... ");
    remove_db();

    yq_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.flags = YQ_OPEN_CREATE;
    opts.page_size = 4096;
    opts.lock_timeout_ms = 0;

    yq_db *a = NULL;
    CHECK_EQ(yq_open(TEST_DB, &opts, &a), YQ_OK);

    yq_txn *ta = NULL;
    CHECK_EQ(yq_txn_begin(a, YQ_TXN_READWRITE, &ta), YQ_OK);  /* holds the lock */

    /* A second writer that does not wait must be rejected immediately. */
    yq_db *b = NULL;
    CHECK_EQ(yq_open(TEST_DB, &opts, &b), YQ_OK);
    yq_txn *tb = NULL;
    CHECK_EQ(yq_txn_begin(b, YQ_TXN_READWRITE, &tb), YQ_ERR_BUSY);
    CHECK_EQ(yq_close(b), YQ_OK);

    /* With a budget it must give up with YQ_ERR_TIMEOUT, not hang. */
    yq_opts waiting = opts;
    waiting.lock_timeout_ms = 30;
    yq_db *c = NULL;
    CHECK_EQ(yq_open(TEST_DB, &waiting, &c), YQ_OK);
    yq_txn *tc = NULL;
    CHECK_EQ(yq_txn_begin(c, YQ_TXN_READWRITE, &tc), YQ_ERR_TIMEOUT);

    /* Readers are never blocked by the writer. */
    yq_txn *ro = NULL;
    CHECK_EQ(yq_txn_begin(c, YQ_TXN_READONLY, &ro), YQ_OK);
    CHECK_EQ(yq_txn_commit(ro), YQ_OK);

    /* Once the writer commits, the waiting handle can take the lock. */
    CHECK_EQ(yq_txn_commit(ta), YQ_OK);
    CHECK_EQ(yq_txn_begin(c, YQ_TXN_READWRITE, &tc), YQ_OK);
    CHECK_EQ(yq_txn_commit(tc), YQ_OK);

    CHECK_EQ(yq_close(c), YQ_OK);
    CHECK_EQ(yq_close(a), YQ_OK);
    remove_db();
    printf("OK\n");
}

/*
 * Values that do not fit in the WAL encode buffer.
 *
 * Regression guard for a stack buffer overflow: yq_wal_append_put() encoded
 * the whole record into a 2048-byte stack buffer and memcpy()'d the value
 * into it unchecked. yq_put() documents values of up to 1 GiB, so anything
 * over ~2 KB wrote past the end of the frame. Sizes are chosen to sit on both
 * sides of the old 2048-byte buffer.
 */
static void test_large_value(void) {
    printf("test_large_value... ");
    remove_db();

    /* 2044 is the largest value that still fits next to a 1-byte key. */
    const size_t sizes[] = { 2044, 2048, 8192, 65536 };
    const int nsizes = (int)(sizeof(sizes) / sizeof(sizes[0]));

    uint8_t **vals = malloc(nsizes * sizeof(uint8_t *));
    CHECK(vals != NULL);
    for (int i = 0; i < nsizes; i++) {
        vals[i] = malloc(sizes[i]);
        CHECK(vals[i] != NULL);
        memset(vals[i], 'a' + i, sizes[i]);
    }

    yq_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.flags = YQ_OPEN_CREATE;
    opts.page_size = 4096;

    yq_db *db = NULL;
    CHECK_EQ(yq_open(TEST_DB, &opts, &db), YQ_OK);

    yq_txn *t = NULL;
    CHECK_EQ(yq_txn_begin(db, YQ_TXN_READWRITE, &t), YQ_OK);
    for (int i = 0; i < nsizes; i++) {
        char k[16];
        snprintf(k, sizeof(k), "big-%d", i);
        yq_slice key = { k, strlen(k) };
        yq_slice val = { vals[i], sizes[i] };
        CHECK_EQ(yq_put(t, key, val, YQ_PUT_UPSERT), YQ_OK);
    }
    CHECK_EQ(yq_txn_commit(t), YQ_OK);
    CHECK_EQ(yq_close(db), YQ_OK);

    /* Reopen: the values must come back out of the WAL intact. */
    db = NULL;
    CHECK_EQ(yq_open(TEST_DB, &opts, &db), YQ_OK);
    t = NULL;
    CHECK_EQ(yq_txn_begin(db, YQ_TXN_READONLY, &t), YQ_OK);
    for (int i = 0; i < nsizes; i++) {
        char k[16];
        snprintf(k, sizeof(k), "big-%d", i);
        yq_slice key = { k, strlen(k) };
        yq_slice out = {0};
        CHECK_EQ(yq_get(t, key, &out), YQ_OK);
        CHECK_EQ((int64_t)out.size, (int64_t)sizes[i]);
        CHECK(memcmp(out.data, vals[i], sizes[i]) == 0);
    }
    CHECK_EQ(yq_txn_commit(t), YQ_OK);

    /* A checkpoint rewrites the log from the memtable; it must not lose or
     * garble the large records either. */
    CHECK_EQ(yq_checkpoint(db), YQ_OK);
    CHECK_EQ(yq_close(db), YQ_OK);

    db = NULL;
    CHECK_EQ(yq_open(TEST_DB, &opts, &db), YQ_OK);
    t = NULL;
    CHECK_EQ(yq_txn_begin(db, YQ_TXN_READONLY, &t), YQ_OK);
    for (int i = 0; i < nsizes; i++) {
        char k[16];
        snprintf(k, sizeof(k), "big-%d", i);
        yq_slice key = { k, strlen(k) };
        yq_slice out = {0};
        CHECK_EQ(yq_get(t, key, &out), YQ_OK);
        CHECK_EQ((int64_t)out.size, (int64_t)sizes[i]);
        CHECK(memcmp(out.data, vals[i], sizes[i]) == 0);
    }
    CHECK_EQ(yq_txn_commit(t), YQ_OK);
    CHECK_EQ(yq_close(db), YQ_OK);

    for (int i = 0; i < nsizes; i++) free(vals[i]);
    free(vals);
    remove_db();

    printf("OK\n");
}

/*
 * Meta block placement.
 *
 * The two meta blocks live in page 0 and page 1 of the database file, i.e. at
 * offsets 0 and opts.page_size. yq_mvcc hard-coded page_size = 4096, so for
 * any other page size meta block 1 was written into the middle of page 0 and
 * page 1 — which the format reserves for it — stayed all zeroes:
 *
 *   $ hexdump db.yqdb          (page_size = 8192)
 *   offset     0: 59 51 44 42   <- meta block 0
 *   offset  4096: 59 51 44 42   <- meta block 1, inside page 0
 *   offset  8192: 00 00 00 00   <- page 1, should hold meta block 1
 *
 * The B+Tree addresses pages by opts.page_size, so page 0 covered both meta
 * blocks, and yq_open() had already reserved npages = 2 for them.
 */
static void test_meta_page_placement(void) {
    printf("test_meta_page_placement... ");

    /*
     * What the writer puts on disk: yq_mvcc stores the magic by memcpy()ing a
     * uint64 into the page, so building the expected bytes the same way keeps
     * this test endian-independent.
     */
    const uint64_t magic = (uint64_t)0x42445159u | ((uint64_t)0x00010A1Au << 32);
    uint8_t want[8];
    for (int i = 0; i < 8; i++) want[i] = (uint8_t)(magic >> (8 * i));

    const uint32_t sizes[] = { 4096, 8192, 16384, 65536 };

    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        const uint32_t ps = sizes[s];
        remove_db();

        yq_opts opts;
        memset(&opts, 0, sizeof(opts));
        opts.struct_size = sizeof(opts);
        opts.flags = YQ_OPEN_CREATE;
        opts.page_size = ps;

        yq_db *db = NULL;
        CHECK_EQ(yq_open(TEST_DB, &opts, &db), YQ_OK);
        yq_txn *t = NULL;
        CHECK_EQ(yq_txn_begin(db, YQ_TXN_READWRITE, &t), YQ_OK);
        yq_slice k = { "k", 1 };
        yq_slice v = { "v", 1 };
        CHECK_EQ(yq_put(t, k, v, YQ_PUT_UPSERT), YQ_OK);
        CHECK_EQ(yq_txn_commit(t), YQ_OK);
        CHECK_EQ(yq_close(db), YQ_OK);

        FILE *f = fopen(TEST_DB, "rb");
        CHECK(f != NULL);
        CHECK(fseek(f, 0, SEEK_END) == 0);
        CHECK(ftell(f) >= (long)ps * 2);   /* at least the two meta pages */

        uint8_t *page = malloc(ps);
        CHECK(page != NULL);

        for (int idx = 0; idx < 2; idx++) {
            CHECK(fseek(f, (long)idx * (long)ps, SEEK_SET) == 0);
            CHECK_EQ((long)fread(page, 1, ps, f), (long)ps);
            CHECK(memcmp(page, want, 8) == 0);
        }

        free(page);
        fclose(f);
    }

    remove_db();
    printf("OK\n");
}

#if defined(YQ_TEST_CAN_MEASURE_HEAP)
/*
 * Bytes currently handed out by malloc(). hblkhd is included because glibc
 * serves large requests (the 64 MiB arena below) from mmap() rather than the
 * main heap, and those would otherwise not show up.
 */
static size_t heap_in_use(void) {
    struct mallinfo2 m = mallinfo2();
    return m.uordblks + m.hblkhd;
}

/*
 * yq_close() must release everything yq_open() allocated.
 *
 * free_db() had an empty `if (db->btree) { }` block, so the yq_btree handle
 * malloc()'d by yq_btree_open()/yq_btree_create() was never freed. Worse,
 * when the database file is empty yq_open() cannot mmap() it and falls back
 * to a heap arena of opts.map_size bytes — 1 GiB by default — which was
 * leaked as well, once per open.
 */
static void test_close_releases_resources(void) {
    printf("test_close_releases_resources... ");
    remove_db();

    yq_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.flags = YQ_OPEN_CREATE;
    opts.page_size = 4096;

    /* Warm up: the first open pulls in stdio/allocator state that would
     * otherwise be counted as growth. */
    for (int i = 0; i < 3; i++) {
        yq_db *d = NULL;
        CHECK_EQ(yq_open(TEST_DB, &opts, &d), YQ_OK);
        CHECK_EQ(yq_close(d), YQ_OK);
    }

    /* (1) the b-tree handle: 96 bytes per cycle, so it takes a few thousand
     *     cycles to show up above allocator noise. */
    size_t heap_before = heap_in_use();
    for (int i = 0; i < 2000; i++) {
        yq_db *d = NULL;
        CHECK_EQ(yq_open(TEST_DB, &opts, &d), YQ_OK);
        CHECK_EQ(yq_close(d), YQ_OK);
    }
    size_t heap_growth = heap_in_use() - heap_before;
    printf("(heap +%zu B) ", heap_growth);
    CHECK(heap_growth < 64 * 1024);   /* leaking: ~220 KB for 2000 cycles */

    /* (2) the heap arena fallback: a zero-length database file cannot be
     *     mmap()ed, so yq_open() malloc()s opts.map_size bytes for the arena.
     *     64 MiB keeps the test cheap; the default is 1 GiB. */
    remove_db();
    FILE *f = fopen(TEST_DB, "wb");
    CHECK(f != NULL);
    fclose(f);

    yq_opts zopts;
    memset(&zopts, 0, sizeof(zopts));
    zopts.struct_size = sizeof(zopts);
    zopts.page_size = 4096;
    zopts.map_size = 64ULL * 1024 * 1024;

    for (int i = 0; i < 2; i++) {
        yq_db *d = NULL;
        CHECK_EQ(yq_open(TEST_DB, &zopts, &d), YQ_OK);
        CHECK_EQ(yq_close(d), YQ_OK);
    }

    size_t arena_before = heap_in_use();
    for (int i = 0; i < 4; i++) {
        yq_db *d = NULL;
        CHECK_EQ(yq_open(TEST_DB, &zopts, &d), YQ_OK);
        CHECK_EQ(yq_close(d), YQ_OK);
    }
    size_t arena_growth = heap_in_use() - arena_before;
    printf("(arena +%zu B) ", arena_growth);
    CHECK(arena_growth < 4ULL * 1024 * 1024);   /* leaking: ~256 MiB here */

    remove_db();
    printf("OK\n");
}
#endif /* YQ_TEST_CAN_MEASURE_HEAP */

#if !defined(_WIN32)
/*
 * Long database paths.
 *
 * Regression guard for the derived-path buffers: yq_open() used to accept a
 * path only up to 1023 bytes and then snprintf() "%s.shm" / "%s.lock" into a
 * 1024-byte buffer (truncating them), while yq_wal_open() rejected anything
 * longer than 507 bytes outright. Both limits were below what yq_open()
 * advertised, so a long path either failed with YQ_ERR_INVAL or silently used
 * truncated auxiliary names.
 */
static void test_long_db_path(void) {
    printf("test_long_db_path... ");

    enum { LEVELS = 6, SEG = 200 };
    char seg[SEG + 1];
    memset(seg, 'd', SEG);
    seg[SEG] = '\0';

    char prefix[LEVELS * (SEG + 1) + 1];
    char levels[LEVELS][LEVELS * (SEG + 1) + 1];
    size_t plen = 0;
    prefix[0] = '\0';

    for (int i = 0; i < LEVELS; i++) {
        memcpy(levels[i], prefix, plen);
        memcpy(levels[i] + plen, seg, SEG);
        memcpy(levels[i] + plen + SEG, "/", 2);
        plen += SEG + 1;
        mkdir(levels[i], 0755);
        memcpy(prefix, levels[i], plen + 1);
    }

    char path[sizeof(prefix) + 32];
    memcpy(path, prefix, plen);
    memcpy(path + plen, "long.yqdb", 10);
    plen += 9;
    CHECK(plen > 1024);   /* past the old buffers */

    yq_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.flags = YQ_OPEN_CREATE;
    opts.page_size = 4096;

    yq_db *db = NULL;
    CHECK_EQ(yq_open(path, &opts, &db), YQ_OK);

    yq_txn *t = NULL;
    CHECK_EQ(yq_txn_begin(db, YQ_TXN_READWRITE, &t), YQ_OK);
    yq_slice k = {"k", 1};
    yq_slice v = {"v", 1};
    CHECK_EQ(yq_put(t, k, v, YQ_PUT_UPSERT), YQ_OK);
    CHECK_EQ(yq_txn_commit(t), YQ_OK);
    CHECK_EQ(yq_close(db), YQ_OK);

    /* The auxiliary files must sit next to the database, not at a truncated
     * path — and the data must survive a reopen. */
    char aux[sizeof(path) + 8];
    memcpy(aux, path, plen);
    memcpy(aux + plen, ".shm", 5);
    CHECK(fopen(aux, "rb") != NULL);
    memcpy(aux + plen, ".lock", 6);
    CHECK(fopen(aux, "rb") != NULL);

    db = NULL;
    CHECK_EQ(yq_open(path, &opts, &db), YQ_OK);
    t = NULL;
    CHECK_EQ(yq_txn_begin(db, YQ_TXN_READONLY, &t), YQ_OK);
    yq_slice out = {0};
    CHECK_EQ(yq_get(t, k, &out), YQ_OK);
    CHECK(out.size == 1);
    CHECK_EQ(yq_txn_commit(t), YQ_OK);
    CHECK_EQ(yq_close(db), YQ_OK);

    memcpy(aux + plen, ".log", 5);  remove(aux);
    memcpy(aux + plen, ".shm", 5);  remove(aux);
    memcpy(aux + plen, ".lock", 6); remove(aux);
    remove(path);
    for (int i = LEVELS - 1; i >= 0; i--) rmdir(levels[i]);

    printf("OK\n");
}
#endif /* !_WIN32 */

/*
 * checkpoint 会把 memtable 里的 value 原样送进 yq_wal_append_put() 重新编码。
 * 该函数历史上用固定的 uint8_t enc_buf[2048]，key 上限 1024 加 varint 开销后，
 * value 超过约 1017 字节就会写穿栈缓冲，而 yq_put() 允许 value 到 1 GiB。
 * 这条用例专门盯住 >1KB 的 value：修复前在 ASan 下会直接
 * "buffer overflow detected"。
 */
static void test_checkpoint_large_value(void) {
    printf("test_checkpoint_large_value... ");
    remove_db();

    yq_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.flags = YQ_OPEN_CREATE;
    opts.page_size = 4096;

    yq_db *db = NULL;
    CHECK_EQ(yq_open(TEST_DB, &opts, &db), YQ_OK);

    /* 16 KiB：远大于旧的 2048 字节栈缓冲，也跨过多条 WAL 扫描块。 */
    const size_t NVAL = 16u * 1024u;
    unsigned char *val = (unsigned char *)malloc(NVAL);
    CHECK(val != NULL);
    for (size_t i = 0; i < NVAL; i++) val[i] = (unsigned char)(i * 7 + 1);

    yq_txn *t = NULL;
    CHECK_EQ(yq_txn_begin(db, YQ_TXN_READWRITE, &t), YQ_OK);
    yq_slice k = {"bigkey", 6};
    yq_slice v = {val, NVAL};
    CHECK_EQ(yq_put(t, k, v, YQ_PUT_UPSERT), YQ_OK);
    CHECK_EQ(yq_txn_commit(t), YQ_OK);

    /* 这一步会把 16 KiB value 重新编码进 WAL。 */
    CHECK_EQ(yq_checkpoint(db), YQ_OK);

    /* 压实前后都必须逐字节一致。 */
    CHECK_EQ(yq_close(db), YQ_OK);
    db = NULL;
    CHECK_EQ(yq_open(TEST_DB, &opts, &db), YQ_OK);
    t = NULL;
    CHECK_EQ(yq_txn_begin(db, YQ_TXN_READONLY, &t), YQ_OK);

    yq_slice out = {0};
    CHECK_EQ(yq_get(t, k, &out), YQ_OK);
    CHECK(out.size == NVAL);
    CHECK(memcmp(out.data, val, NVAL) == 0);

    CHECK_EQ(yq_txn_commit(t), YQ_OK);
    CHECK_EQ(yq_close(db), YQ_OK);
    free(val);
    remove_db();
    printf("OK\n");
}

/*
 * Key/value limits must be reported with YQ_ERR_TOOBIG on every write path
 * (ERRORS.md 4.2). yq_put() did, but yq_del() answered YQ_ERR_INVAL for the
 * same oversized key, so callers could not distinguish "too big" from
 * "bad argument" without consulting the size again.
 */
static void test_key_value_limits(void) {
    printf("test_key_value_limits... ");
    remove_db();

    yq_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.flags = YQ_OPEN_CREATE;

    yq_db *db = NULL;
    CHECK_EQ(yq_open(TEST_DB, &opts, &db), YQ_OK);

    yq_txn *txn = NULL;
    CHECK_EQ(yq_txn_begin(db, YQ_TXN_READWRITE, &txn), YQ_OK);

    static uint8_t big[1025];
    memset(big, 'x', sizeof(big));
    yq_slice big_key = { big, sizeof(big) };
    yq_slice empty_key = { "", 0 };

    CHECK_EQ(yq_put(txn, big_key, big_key, 0), YQ_ERR_TOOBIG);
    CHECK_EQ(yq_put(txn, empty_key, big_key, 0), YQ_ERR_TOOBIG);
    CHECK_EQ(yq_del(txn, big_key), YQ_ERR_TOOBIG);
    CHECK_EQ(yq_del(txn, empty_key), YQ_ERR_TOOBIG);

    /* A valid key right at the limit must still be accepted. */
    yq_slice max_key = { big, 1024 };
    CHECK_EQ(yq_put(txn, max_key, max_key, 0), YQ_OK);

    CHECK_EQ(yq_txn_abort(txn), YQ_OK);
    CHECK_EQ(yq_close(db), YQ_OK);
    remove_db();
    printf("OK\n");
}
/*
 * The pool keeps one free list per size class (regression guard).
 *
 * The pool used to keep a single mixed-size free list: free() pushed any
 * object onto it and alloc() popped any object off it, so a 256-byte
 * request could be handed a slot that had been freed after a 64-byte
 * allocation. Filling that object then ran past the slot into its
 * neighbour. Both neighbours are filled with known patterns here and must
 * survive any fill of the larger object.
 */
static void test_mempool_size_classes(void) {
    printf("test_mempool_size_classes... ");

    yq_mempool *pool = yq_mempool_create();
    CHECK(pool != NULL);

    unsigned char *a = (unsigned char *)yq_mempool_alloc(pool, 64);
    unsigned char *b = (unsigned char *)yq_mempool_alloc(pool, 64);
    CHECK(a != NULL && b != NULL);
    memset(a, 0xAA, 64);
    memset(b, 0xBB, 64);

    yq_mempool_free(pool, a);

    /* A 256-byte request must not be served the 64-byte slot. */
    unsigned char *c = (unsigned char *)yq_mempool_alloc(pool, 256);
    CHECK(c != NULL);
    CHECK(c != a);
    memset(c, 0xCC, 256);

    int clobbered = 0;
    for (int i = 0; i < 64; i++) {
        if (b[i] != 0xBB) clobbered++;
    }
    CHECK(clobbered == 0);

    /* Same-class reuse still works: the next 64-byte request takes the slot. */
    unsigned char *d = (unsigned char *)yq_mempool_alloc(pool, 64);
    CHECK(d == a);
    memset(d, 0xDD, 64);
    clobbered = 0;
    for (int i = 0; i < 64; i++) {
        if (b[i] != 0xBB) clobbered++;
    }
    CHECK(clobbered == 0);

    yq_mempool_stats st;
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_EQ(yq_mempool_stats_get(pool, &st), YQ_OK);
    CHECK(st.chunks_allocated == 1);
    CHECK(st.objects_allocated == 3);
    CHECK(st.objects_freed == 1);
    CHECK(st.free_objects == 0);

    yq_mempool_destroy(pool);
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
    test_checkpoint_compaction();
    test_writer_lock_contention();
    test_large_value();
    test_meta_page_placement();
#if defined(YQ_TEST_CAN_MEASURE_HEAP)
    test_close_releases_resources();
#endif
#if !defined(_WIN32)
    test_long_db_path();
#endif
    test_checkpoint_large_value();
    test_key_value_limits();
    test_nosync();
    test_mempool_size_classes();

    printf("\n=== ALL TESTS PASSED ===\n");
    return 0;
}
