/*
 * yq_test_mini — minimal end-to-end smoke test.
 *
 * Covers the shortest path through the public ABI that still crosses a
 * process boundary: open -> write -> commit -> close -> reopen -> read.
 * It is deliberately dependency-light (no clocks, no threads) so it can be
 * used as a fast sanity check on a new toolchain before running the heavier
 * integration suite.
 *
 * Assertions go through CHECK/CHECK_EQ from yq_test_check.h, never through
 * assert(): CMake adds -DNDEBUG in Release-like build types, which turns
 * assert() into a no-op, so a print-only or assert-based version prints its
 * OK lines and exits 0 even when the engine is broken. CHECK() aborts with a
 * non-zero status in every build type, which is what makes this test
 * meaningful under `ctest -C Release`.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yq.h"
#include "yq_test_check.h"

static const char *TEST_DB = "yqtest_mini.yqdb";

static void remove_db(void) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s.log", TEST_DB);  remove(buf);
    snprintf(buf, sizeof(buf), "%s.shm", TEST_DB);  remove(buf);
    snprintf(buf, sizeof(buf), "%s.lock", TEST_DB); remove(buf);
    remove(TEST_DB);
}

static void open_opts(yq_opts *opts) {
    memset(opts, 0, sizeof(*opts));
    opts->struct_size = sizeof(*opts);
    opts->flags = YQ_OPEN_CREATE;
    opts->page_size = 4096;
    opts->memtable_bytes = 1024 * 1024;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    remove_db();

    printf("yq_version... ");
    int ma = 0, mi = 0, pa = 0;
    CHECK_EQ(yq_version(&ma, &mi, &pa), YQ_OK);
    CHECK(ma == YQ_VERSION_MAJOR && mi == YQ_VERSION_MINOR && pa == YQ_VERSION_PATCH);
    printf("OK (v%d.%d.%d)\n", ma, mi, pa);

    yq_opts opts;
    open_opts(&opts);

    printf("yq_open... ");
    yq_db *db = NULL;
    CHECK_EQ(yq_open(TEST_DB, &opts, &db), YQ_OK);
    CHECK(db != NULL);
    printf("OK\n");

    printf("yq_txn_begin... ");
    yq_txn *txn = NULL;
    CHECK_EQ(yq_txn_begin(db, YQ_TXN_READWRITE, &txn), YQ_OK);
    CHECK(txn != NULL);
    printf("OK\n");

    printf("yq_put / yq_get... ");
    yq_slice k = { .data = (const void *)"hello", .size = 5 };
    yq_slice v = { .data = (const void *)"world", .size = 5 };
    CHECK_EQ(yq_put(txn, k, v, YQ_PUT_UPSERT), YQ_OK);

    yq_slice out = {0};
    CHECK_EQ(yq_get(txn, k, &out), YQ_OK);
    CHECK(out.size == 5);
    CHECK(out.data != NULL && memcmp(out.data, "world", 5) == 0);
    printf("OK\n");

    printf("yq_txn_commit... ");
    CHECK_EQ(yq_txn_commit(txn), YQ_OK);
    printf("OK\n");

    printf("yq_close... ");
    CHECK_EQ(yq_close(db), YQ_OK);
    printf("OK\n");

    /* Reopen: the committed write must survive through WAL recovery. This is
     * the step that actually proves durability, so it is asserted too. */
    printf("reopen and read back... ");
    db = NULL;
    CHECK_EQ(yq_open(TEST_DB, &opts, &db), YQ_OK);
    txn = NULL;
    CHECK_EQ(yq_txn_begin(db, YQ_TXN_READONLY, &txn), YQ_OK);
    out.data = NULL;
    out.size = 0;
    CHECK_EQ(yq_get(txn, k, &out), YQ_OK);
    CHECK(out.size == 5);
    CHECK(out.data != NULL && memcmp(out.data, "world", 5) == 0);
    CHECK_EQ(yq_txn_commit(txn), YQ_OK);
    CHECK_EQ(yq_close(db), YQ_OK);
    printf("OK\n");

    remove_db();

    printf("MINI SMOKE TEST PASSED\n");
    return 0;
}
