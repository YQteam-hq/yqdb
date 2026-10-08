#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include "yq.h"

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
    assert(rc == YQ_OK);
    assert(db != NULL);

    rc = yq_close(db);
    assert(rc == YQ_OK);

    rc = yq_open(TEST_DB, &opts, &db);
    assert(rc == YQ_OK);

    rc = yq_close(db);
    assert(rc == YQ_OK);

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
    assert(rc == YQ_OK);

    yq_txn *txn = NULL;
    rc = yq_txn_begin(db, YQ_TXN_READWRITE, &txn);
    assert(rc == YQ_OK);

    yq_slice key = {"k1", 2};
    yq_slice val = {"v1", 2};
    rc = yq_put(txn, key, val, 0);
    assert(rc == YQ_OK);

    yq_slice out = {0};
    rc = yq_get(txn, key, &out);
    assert(rc == YQ_OK);
    assert(out.size == 2);
    assert(memcmp(out.data, "v1", 2) == 0);

    rc = yq_txn_commit(txn);
    assert(rc == YQ_OK);

    rc = yq_close(db);
    assert(rc == YQ_OK);

    rc = yq_open(TEST_DB, &opts, &db);
    assert(rc == YQ_OK);

    rc = yq_txn_begin(db, YQ_TXN_READONLY, &txn);
    assert(rc == YQ_OK);

    rc = yq_get(txn, key, &out);
    assert(rc == YQ_OK);
    assert(out.size == 2);

    rc = yq_txn_commit(txn);
    assert(rc == YQ_OK);

    rc = yq_close(db);
    assert(rc == YQ_OK);

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
    assert(rc == YQ_OK);

    yq_del(txn, k1);
    rc = yq_get(txn, k1, &out);
    assert(rc == YQ_ERR_NOTFOUND);

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
    assert(rc == YQ_OK);

    int count = 0;
    for (rc = yq_cur_first(cur); rc == YQ_OK; rc = yq_cur_next(cur)) {
        yq_slice k = {0}, v = {0};
        yq_cur_key(cur, &k);
        yq_cur_val(cur, &v);
        assert(k.size > 0);
        assert(v.size > 0);
        count++;
    }
    assert(count == 20);

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
    assert(rc == YQ_ERR_NOTFOUND);
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
    assert(rc == YQ_ERR_EXISTS);

    yq_slice out = {0};
    yq_get(txn, k1, &out);
    assert(out.size == 4);
    assert(memcmp(out.data, "val1", 4) == 0);

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
    assert(rc == YQ_OK);

    rc = yq_sync(db);
    assert(rc == YQ_OK);

    yq_txn_begin(db, YQ_TXN_READONLY, &txn);
    int count = 0;
    yq_cur *cur = NULL;
    yq_cur_open(txn, &cur);
    for (rc = yq_cur_first(cur); rc == YQ_OK; rc = yq_cur_next(cur)) {
        count++;
    }
    yq_cur_close(cur);
    yq_txn_commit(txn);
    assert(count == 100);

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
    assert(rc == YQ_OK);

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
    assert(count == 1000);

    double t2 = now_sec();
    printf("(%.1f gets/sec) ", 1000.0 / (t2 - t1));

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
    assert(total_got == 250);

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
    assert(rc == YQ_OK);
    assert(st.struct_size == sizeof(yq_stat));
    assert(st.format_version == 1);
    assert(st.page_size == 4096);

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
    assert(strcmp(yq_strerror(YQ_OK), "success") == 0);
    assert(strcmp(yq_strerror(YQ_ERR_NOMEM), "out of memory") == 0);
    assert(strcmp(yq_strerror(YQ_ERR_NOTFOUND), "key not found") == 0);
    assert(strcmp(yq_strerror(99), "unknown error") == 0);
    printf("OK\n");
}

static void test_version(void) {
    printf("test_version... ");
    int major = -1, minor = -1, patch = -1;
    int rc = yq_version(&major, &minor, &patch);
    assert(rc == YQ_OK);
    assert(major == 1);
    assert(minor == 0);
    assert(patch == 0);
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
    test_stat();
    test_nosync();

    printf("\n=== ALL TESTS PASSED ===\n");
    return 0;
}
