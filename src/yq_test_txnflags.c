/*
 * yq_test_txnflags.c -- yq_txn_begin() flag validation.
 *
 * flags is a closed enum with exactly two members, YQ_TXN_READWRITE (0x0001)
 * and YQ_TXN_READONLY (0x0002). The old implementation treated it as
 * "read-write if bit 0 is set, read-only otherwise", so:
 *
 *   yq_txn_begin(db, 0, &txn)   returned YQ_OK and a *read-only* transaction,
 *
 * and the only symptom showed up later, as YQ_ERR_READONLY from the first
 * yq_put(). Passing 0 looks like "no flags, take the defaults" to a caller, so
 * that failure is hard to trace back. Unknown bits (a future flag, a mistyped
 * constant) were ignored the same way.
 *
 * The tightening also removes a latent inconsistency: yq_put()/yq_del() decide
 * read-only with `!(flags & YQ_TXN_READWRITE)` while yq_batch_put()/
 * yq_batch_del() use `flags & YQ_TXN_READONLY`. For flags == 0 the two disagree
 * -- the batch guard passes. Only the fact that the batch path delegates each
 * entry to yq_put() kept a write from slipping through. With just the two
 * documented values, every predicate is exact.
 *
 * This test pins the rejection of ambiguous flags and checks that the two valid
 * values still behave as documented.
 */

#include <stdio.h>
#include <string.h>

#include "yq.h"

static const char *TEST_DB = "yqtest_txnflags.yqdb";

static int g_fail = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("        FAIL: %s (line %d)\n", #cond, __LINE__);           \
            g_fail++;                                                          \
        }                                                                      \
    } while (0)

static void remove_db_files(void) {
    char buf[128];
    snprintf(buf, sizeof buf, "%s.log", TEST_DB);  remove(buf);
    snprintf(buf, sizeof buf, "%s.shm", TEST_DB);  remove(buf);
    snprintf(buf, sizeof buf, "%s.lock", TEST_DB); remove(buf);
    remove(TEST_DB);
}

static yq_db *open_rw(void) {
    remove_db_files();
    yq_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.struct_size = sizeof opts;
    opts.flags = YQ_OPEN_CREATE;
    yq_db *db = NULL;
    if (yq_open(TEST_DB, &opts, &db) != YQ_OK) return NULL;
    return db;
}

/* ── ambiguous / undefined flag values are rejected ──────────────────────── */

static void test_rejects_ambiguous_flags(void) {
    printf("test_rejects_ambiguous_flags... ");
    int before = g_fail;
    yq_db *db = open_rw();
    if (!db) {
        printf("FAIL (fixture)\n");
        g_fail++;
        return;
    }

    const uint32_t bad[] = {
        0u,                                        /* neither constant */
        YQ_TXN_READWRITE | YQ_TXN_READONLY,        /* both */
        0x04u,                                     /* undefined bit */
        0x80000000u,                               /* undefined high bit */
        YQ_TXN_READWRITE | 0x10u,                  /* valid bit + junk */
        0xFFFFFFFFu,
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        /* Non-NULL poison: yq_txn_begin() documents that it writes *out on
         * every path, so a rejected call must still leave NULL behind. */
        yq_txn *txn = (yq_txn *)(uintptr_t)1;
        int rc = yq_txn_begin(db, bad[i], &txn);
        if (rc != YQ_ERR_INVAL) {
            printf("\n        FAIL: flags=0x%x -> %d, want YQ_ERR_INVAL (%d)\n",
                   bad[i], rc, YQ_ERR_INVAL);
            g_fail++;
        }
        if (txn != NULL) {
            printf("\n        FAIL: flags=0x%x left *out non-NULL\n", bad[i]);
            g_fail++;
        }
    }

    yq_close(db);
    printf("%s\n", g_fail == before ? "PASS" : "FAIL");
}

/* ── the two documented values still do what the header says ─────────────── */

static void test_valid_flags_still_work(void) {
    printf("test_valid_flags_still_work... ");
    int before = g_fail;
    yq_db *db = open_rw();
    if (!db) {
        printf("FAIL (fixture)\n");
        g_fail++;
        return;
    }

    /* read-write: writes are allowed and visible after commit */
    yq_txn *rw = NULL;
    CHECK(yq_txn_begin(db, YQ_TXN_READWRITE, &rw) == YQ_OK);
    yq_slice key = { "k", 1 };
    yq_slice val = { "v", 1 };
    CHECK(rw != NULL && yq_put(rw, key, val, YQ_PUT_UPSERT) == YQ_OK);
    CHECK(yq_txn_commit(rw) == YQ_OK);

    /* read-only: reads succeed, writes report YQ_ERR_READONLY */
    yq_txn *ro = NULL;
    CHECK(yq_txn_begin(db, YQ_TXN_READONLY, &ro) == YQ_OK);
    yq_slice got = { 0 };
    CHECK(ro != NULL && yq_get(ro, key, &got) == YQ_OK);
    CHECK(got.size == 1 && ((const char *)got.data)[0] == 'v');
    CHECK(yq_put(ro, key, val, YQ_PUT_UPSERT) == YQ_ERR_READONLY);
    CHECK(yq_del(ro, key) == YQ_ERR_READONLY);
    CHECK(yq_txn_commit(ro) == YQ_OK);

    /* read-only transactions nest; the header promises that */
    yq_txn *a = NULL, *b = NULL;
    CHECK(yq_txn_begin(db, YQ_TXN_READONLY, &a) == YQ_OK);
    CHECK(yq_txn_begin(db, YQ_TXN_READONLY, &b) == YQ_OK);
    CHECK(a != NULL && b != NULL);
    yq_txn_commit(b);
    yq_txn_commit(a);

    yq_close(db);
    printf("%s\n", g_fail == before ? "PASS" : "FAIL");
}

/* ── a read-only handle refuses a write transaction up front ─────────────── */

static void test_readonly_handle_rejects_write_txn(void) {
    printf("test_readonly_handle_rejects_write_txn... ");
    int before = g_fail;
    yq_db *db = open_rw();
    if (!db) {
        printf("FAIL (fixture)\n");
        g_fail++;
        return;
    }
    yq_close(db);

    yq_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.struct_size = sizeof opts;
    opts.flags = YQ_OPEN_READONLY;
    yq_db *ro_db = NULL;
    CHECK(yq_open(TEST_DB, &opts, &ro_db) == YQ_OK);

    yq_txn *txn = NULL;
    CHECK(yq_txn_begin(ro_db, YQ_TXN_READWRITE, &txn) == YQ_ERR_READONLY);
    CHECK(txn == NULL);

    /* ... while a read-only transaction on the same handle is fine */
    CHECK(yq_txn_begin(ro_db, YQ_TXN_READONLY, &txn) == YQ_OK);
    if (txn) yq_txn_commit(txn);

    yq_close(ro_db);
    printf("%s\n", g_fail == before ? "PASS" : "FAIL");
}

int main(void) {
    test_rejects_ambiguous_flags();
    test_valid_flags_still_work();
    test_readonly_handle_rejects_write_txn();

    remove_db_files();

    if (g_fail) {
        printf("\ntxnflags_test: %d check(s) FAILED\n", g_fail);
        return 1;
    }
    printf("\ntxnflags_test: txn flags are a closed enum\n");
    return 0;
}
