/*
 * Regression tests for write protection on a YQ_OPEN_READONLY handle.
 *
 * yq_txn_begin() never checked db->write_enabled, so a handle opened read-only
 * happily handed out a YQ_TXN_READWRITE transaction; yq_put()/yq_del() only
 * look at the transaction flag, so the writes went through and commit wrote
 * the meta pages and the log. The documented guarantee that a read-only open
 * "does not modify the main database file" did not hold.
 *
 * yq_checkpoint() and yq_sync() did refuse, but reported YQ_ERR_CORRUPT --
 * which per ERRORS.md means a CRC or format failure, not a read-only handle.
 *
 * Checks use an explicit macro instead of assert() so that they stay active
 * regardless of whether the build type defines NDEBUG.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yq.h"

static const char *TEST_DB = "yqtest_readonly.yqdb";

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            exit(1);                                                      \
        }                                                                 \
    } while (0)

static void remove_db(void) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s.log", TEST_DB);  remove(buf);
    snprintf(buf, sizeof(buf), "%s.shm", TEST_DB);  remove(buf);
    snprintf(buf, sizeof(buf), "%s.lock", TEST_DB); remove(buf);
    remove(TEST_DB);
}

static void open_db(yq_db **db, uint32_t flags) {
    yq_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.flags = flags;

    CHECK(yq_open(TEST_DB, &opts, db) == YQ_OK);
    CHECK(*db != NULL);
}

/* Create the database so the read-only cases have something to open. */
static void create_db(void) {
    yq_db *db = NULL;
    open_db(&db, YQ_OPEN_CREATE);
    CHECK(yq_close(db) == YQ_OK);
}

static void test_readwrite_txn_rejected(void) {
    printf("test_readwrite_txn_rejected... ");
    remove_db();
    create_db();

    yq_db *db = NULL;
    open_db(&db, YQ_OPEN_READONLY);

    yq_txn *txn = (yq_txn *)(void *)TEST_DB; /* must be overwritten */
    int rc = yq_txn_begin(db, YQ_TXN_READWRITE, &txn);
    CHECK(rc == YQ_ERR_READONLY);
    CHECK(txn == NULL);

    CHECK(yq_close(db) == YQ_OK);
    remove_db();
    printf("OK\n");
}

/* Read-only transactions stay available on a read-only handle. */
static void test_readonly_txn_allowed(void) {
    printf("test_readonly_txn_allowed... ");
    remove_db();
    create_db();

    yq_db *db = NULL;
    open_db(&db, YQ_OPEN_READONLY);

    yq_txn *txn = NULL;
    int rc = yq_txn_begin(db, YQ_TXN_READONLY, &txn);
    CHECK(rc != YQ_ERR_READONLY);
    if (rc == YQ_OK) {
        yq_slice key = {"absent", 6};
        yq_slice out = {0};
        CHECK(yq_get(txn, key, &out) == YQ_ERR_NOTFOUND);
        CHECK(yq_txn_commit(txn) == YQ_OK);
    }

    CHECK(yq_close(db) == YQ_OK);
    remove_db();
    printf("OK\n");
}

static void test_maintenance_rejected(void) {
    printf("test_maintenance_rejected... ");
    remove_db();
    create_db();

    yq_db *db = NULL;
    open_db(&db, YQ_OPEN_READONLY);

    CHECK(yq_checkpoint(db) == YQ_ERR_READONLY);
    CHECK(yq_sync(db) == YQ_ERR_READONLY);

    CHECK(yq_close(db) == YQ_OK);
    remove_db();
    printf("OK\n");
}

/* A writable handle must be unaffected by the new guard. */
static void test_writable_handle_unaffected(void) {
    printf("test_writable_handle_unaffected... ");
    remove_db();

    yq_db *db = NULL;
    open_db(&db, YQ_OPEN_CREATE);

    CHECK(yq_sync(db) == YQ_OK);
    CHECK(yq_checkpoint(db) == YQ_OK);

    yq_txn *txn = NULL;
    int rc = yq_txn_begin(db, YQ_TXN_READWRITE, &txn);
    CHECK(rc != YQ_ERR_READONLY);
    if (rc == YQ_OK) {
        yq_slice key = {"k", 1};
        yq_slice val = {"v", 1};
        CHECK(yq_put(txn, key, val, YQ_PUT_UPSERT) == YQ_OK);
        CHECK(yq_txn_commit(txn) == YQ_OK);
    }

    CHECK(yq_close(db) == YQ_OK);
    remove_db();
    printf("OK\n");
}

static void test_null_handle(void) {
    printf("test_null_handle... ");
    CHECK(yq_checkpoint(NULL) == YQ_ERR_INVAL);
    CHECK(yq_sync(NULL) == YQ_ERR_INVAL);
    printf("OK\n");
}

int main(void) {
    printf("=== yq-DB Read-Only Handle Tests ===\n\n");

    test_null_handle();
    test_readwrite_txn_rejected();
    test_readonly_txn_allowed();
    test_maintenance_rejected();
    test_writable_handle_unaffected();

    printf("\n=== ALL TESTS PASSED ===\n");
    return 0;
}
