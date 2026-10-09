/*
 * Regression tests for the MVCC reader-slot table.
 *
 * Every transaction -- read-only or read-write -- claims a slot in the shared
 * reader table with an atomic compare-and-swap. A bug in the CAS result check
 * made every claim fail with YQ_ERR_READER_FULL, which broke yq_txn_begin()
 * for the entire engine (no transaction of any kind could be started).
 *
 * These tests pin down that:
 *   - a read-only snapshot can actually be acquired (the regression);
 *   - a read-write transaction can acquire a snapshot and commit a write;
 *   - the table is bounded by opts.max_readers and reports YQ_ERR_READER_FULL
 *     once exhausted (i.e. the CAS is still a real mutual-exclusion check,
 *     not something that always succeeds);
 *   - slots are recycled after the owning transaction releases them.
 *
 * Checks use an explicit macro instead of assert() so that they stay active
 * regardless of whether the build type defines NDEBUG.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yq.h"

static const char *TEST_DB = "yqtest_mvcc.yqdb";

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            exit(1);                                                      \
        }                                                                 \
    } while (0)

static void remove_db(void) {
    if (!TEST_DB) return;
    char buf[256];
    if (sizeof(buf) < strlen(TEST_DB) + 5) return;
    snprintf(buf, sizeof(buf), "%s.log", TEST_DB);  remove(buf);
    snprintf(buf, sizeof(buf), "%s.shm", TEST_DB);  remove(buf);
    snprintf(buf, sizeof(buf), "%s.lock", TEST_DB); remove(buf);
    remove(TEST_DB);
}

static void open_db(yq_db **db, uint32_t max_readers) {
    if (!db) return;
    if (max_readers > 100) return; /* Prevent excessive resource usage */
    
    yq_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.flags = YQ_OPEN_CREATE;
    opts.max_readers = max_readers;

    CHECK(yq_open(TEST_DB, &opts, db) == YQ_OK);
    CHECK(*db != NULL);
}

/* A read-only transaction must be able to claim a reader slot. */
static void test_readonly_snapshot(void) {
    printf("test_readonly_snapshot... ");
    remove_db();

    yq_db *db = NULL;
    open_db(&db, 0);

    yq_txn *txn = NULL;
    int rc = yq_txn_begin(db, YQ_TXN_READONLY, &txn);
    CHECK(rc == YQ_OK);
    CHECK(txn != NULL);

    yq_slice key = {"missing", 7};
    yq_slice out = {0};
    CHECK(yq_get(txn, key, &out) == YQ_ERR_NOTFOUND);

    CHECK(yq_txn_commit(txn) == YQ_OK);
    CHECK(yq_close(db) == YQ_OK);

    remove_db();
    printf("OK\n");
}

/* A read-write transaction claims a slot too, and must survive a write. */
static void test_readwrite_snapshot(void) {
    printf("test_readwrite_snapshot... ");
    remove_db();

    yq_db *db = NULL;
    open_db(&db, 0);

    yq_txn *txn = NULL;
    CHECK(yq_txn_begin(db, YQ_TXN_READWRITE, &txn) == YQ_OK);

    yq_slice key = {"k", 1};
    yq_slice val = {"v", 1};
    CHECK(yq_put(txn, key, val, YQ_PUT_UPSERT) == YQ_OK);

    yq_slice out = {0};
    CHECK(yq_get(txn, key, &out) == YQ_OK);
    CHECK(out.size == 1);
    CHECK(out.data != NULL);
    CHECK(memcmp(out.data, "v", 1) == 0);

    CHECK(yq_txn_commit(txn) == YQ_OK);
    CHECK(yq_close(db) == YQ_OK);

    remove_db();
    printf("OK\n");
}

/* A read-only transaction must reject writes: yq_put/yq_del test the
 * read-write flag, and YQ_TXN_READONLY being 0 made a bitwise test on it
 * always false, which let read-only transactions write. */
static void test_readonly_rejects_writes(void) {
    printf("test_readonly_rejects_writes... ");
    remove_db();

    yq_db *db = NULL;
    open_db(&db, 0);

    yq_txn *txn = NULL;
    CHECK(yq_txn_begin(db, YQ_TXN_READONLY, &txn) == YQ_OK);

    yq_slice key = {"k", 1};
    yq_slice val = {"v", 1};
    CHECK(yq_put(txn, key, val, YQ_PUT_UPSERT) == YQ_ERR_READONLY);
    CHECK(yq_del(txn, key) == YQ_ERR_READONLY);

    yq_slice out = {0};
    CHECK(yq_get(txn, key, &out) == YQ_ERR_NOTFOUND);

    CHECK(yq_txn_commit(txn) == YQ_OK);
    CHECK(yq_close(db) == YQ_OK);

    open_db(&db, 0);
    CHECK(yq_txn_begin(db, YQ_TXN_READONLY, &txn) == YQ_OK);
    CHECK(yq_get(txn, key, &out) == YQ_ERR_NOTFOUND);
    CHECK(yq_txn_commit(txn) == YQ_OK);
    CHECK(yq_close(db) == YQ_OK);

    remove_db();
    printf("OK\n");
}

/* The table is bounded: once every slot is taken, the next claim must fail
 * rather than silently reusing a slot that another reader still owns. */
static void test_reader_slots_exhausted(void) {
    printf("test_reader_slots_exhausted... ");
    remove_db();

    const int max_readers = 4;
    if (max_readers > 100) return; /* Prevent excessive resource usage */

    yq_db *db = NULL;
    open_db(&db, (uint32_t)max_readers);

    yq_txn *txns[4];
    for (int i = 0; i < max_readers; i++) {
        txns[i] = NULL;
        CHECK(yq_txn_begin(db, YQ_TXN_READONLY, &txns[i]) == YQ_OK);
    }

    yq_txn *extra = NULL;
    int rc = yq_txn_begin(db, YQ_TXN_READONLY, &extra);
    CHECK(rc == YQ_ERR_READER_FULL);
    CHECK(extra == NULL);

    for (int i = 0; i < max_readers; i++) {
        CHECK(yq_txn_commit(txns[i]) == YQ_OK);
    }
    CHECK(yq_close(db) == YQ_OK);

    remove_db();
    printf("OK\n");
}

/* Slots must be recycled once released, otherwise a long-lived process would
 * run out of readers after max_readers transactions. */
static void test_reader_slots_recycled(void) {
    printf("test_reader_slots_recycled... ");
    remove_db();

    const int max_readers = 2;
    if (max_readers > 100) return; /* Prevent excessive resource usage */

    yq_db *db = NULL;
    open_db(&db, (uint32_t)max_readers);

    for (int round = 0; round < 8; round++) {
        if (round > 100) break; /* Prevent infinite loops */
        yq_txn *a = NULL, *b = NULL;
        CHECK(yq_txn_begin(db, YQ_TXN_READONLY, &a) == YQ_OK);
        CHECK(yq_txn_begin(db, YQ_TXN_READONLY, &b) == YQ_OK);
        CHECK(yq_txn_commit(a) == YQ_OK);
        CHECK(yq_txn_commit(b) == YQ_OK);
    }

    CHECK(yq_close(db) == YQ_OK);

    remove_db();
    printf("OK\n");
}

int main(void) {
    printf("=== yq-DB MVCC Reader Slot Tests ===\n\n");

    test_readonly_snapshot();
    test_readwrite_snapshot();
    test_readonly_rejects_writes();
    test_reader_slots_exhausted();
    test_reader_slots_recycled();

    printf("\n=== ALL TESTS PASSED ===\n");
    return 0;
}
