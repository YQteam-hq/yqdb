/*
 * yq_test_putargs.c -- argument validation for the write path.
 *
 * The engine used to classify key errors differently at each entry point:
 *
 *   entry            key.size == 0     key.size == 1025
 *   yq_put()         YQ_ERR_TOOBIG     YQ_ERR_TOOBIG
 *   yq_del()         YQ_ERR_INVAL      YQ_ERR_INVAL
 *   yq_batch_put()   YQ_ERR_INVAL      YQ_ERR_INVAL
 *   yq_batch_del()   YQ_ERR_INVAL      YQ_ERR_INVAL
 *
 * ERRORS.md only ever documented two rules -- an over-long key is TOOBIG, and a
 * malformed argument is INVAL -- so no single call site matched the document and
 * callers could not classify a failure without knowing which API they had used.
 * An empty key is not "too big"; it is not a key at all.
 *
 * yq_put() had a second hole: `mode` is a closed enum (UPSERT / NOOVERWRITE) but
 * any other value fell through to the UPSERT branch, so `mode = 2` or a stray
 * high bit silently overwrote existing data while the caller believed it had
 * asked for something else.
 *
 * This test pins the corrected, documented behaviour and -- just as important --
 * checks that a rejected call mutates nothing.
 */

#include <stdio.h>
#include <string.h>

#include "yq.h"

static int g_fail = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("        FAIL: %s (line %d)\n", #cond, __LINE__);           \
            g_fail++;                                                          \
        }                                                                      \
    } while (0)

#define CHECK_RC(expr, want)                                                   \
    do {                                                                       \
        int _rc = (expr);                                                      \
        if (_rc != (want)) {                                                   \
            printf("        FAIL: %s -> %d, want %d (%s)\n", #expr, _rc,       \
                   (want), yq_strerror(want));                                 \
            g_fail++;                                                          \
        }                                                                      \
    } while (0)

static const char *TEST_DB = "yqtest_putargs.yqdb";

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
    int rc = yq_open(TEST_DB, &opts, &db);
    if (rc != YQ_OK) {
        printf("        FATAL: yq_open failed: %d (%s)\n", rc, yq_strerror(rc));
        return NULL;
    }
    return db;
}

static int count_keys(yq_db *db) {
    yq_txn *txn = NULL;
    if (yq_txn_begin(db, YQ_TXN_READONLY, &txn) != YQ_OK) return -1;
    yq_cur *cur = NULL;
    int n = 0;
    if (yq_cur_open(txn, &cur) == YQ_OK) {
        for (int r = yq_cur_first(cur); r == YQ_OK; r = yq_cur_next(cur)) n++;
        yq_cur_close(cur);
    }
    yq_txn_commit(txn);
    return n;
}

/* ── empty key: INVAL everywhere, because it is not a size-limit violation ── */

static void test_empty_key_is_inval(void) {
    printf("test_empty_key_is_inval... ");
    int before = g_fail;
    yq_db *db = open_rw();
    if (!db) return;

    yq_txn *txn = NULL;
    CHECK_RC(yq_txn_begin(db, YQ_TXN_READWRITE, &txn), YQ_OK);

    yq_slice empty = { "", 0 };
    yq_slice val = { "v", 1 };
    CHECK_RC(yq_put(txn, empty, val, YQ_PUT_UPSERT), YQ_ERR_INVAL);
    CHECK_RC(yq_put(txn, empty, val, YQ_PUT_NOOVERWRITE), YQ_ERR_INVAL);
    CHECK_RC(yq_del(txn, empty), YQ_ERR_INVAL);

    /* A slice with no storage is malformed whatever the length field says, and
     * it must be rejected before anything dereferences it. Both spellings are
     * INVAL, and neither depends on which check fires first. */
    yq_slice null_zero = { NULL, 0 };
    CHECK_RC(yq_put(txn, null_zero, val, YQ_PUT_UPSERT), YQ_ERR_INVAL);
    yq_slice null_sized = { NULL, 5 };
    CHECK_RC(yq_put(txn, null_sized, val, YQ_PUT_UPSERT), YQ_ERR_INVAL);
    CHECK_RC(yq_del(txn, null_sized), YQ_ERR_INVAL);

    CHECK_RC(yq_txn_commit(txn), YQ_OK);
    CHECK(count_keys(db) == 0); /* nothing was written */
    yq_close(db);
    printf("%s\n", g_fail == before ? "PASS" : "FAIL");
}

/* ── 1024 is the limit; 1025 is over it ──────────────────────────────────── */

static void test_key_size_limit(void) {
    printf("test_key_size_limit... ");
    int before = g_fail;
    yq_db *db = open_rw();
    if (!db) return;

    static char buf[1025];
    memset(buf, 'k', sizeof buf);

    yq_txn *txn = NULL;
    CHECK_RC(yq_txn_begin(db, YQ_TXN_READWRITE, &txn), YQ_OK);

    yq_slice val = { "v", 1 };
    yq_slice at_limit = { buf, 1024 };
    yq_slice over_limit = { buf, 1025 };

    CHECK_RC(yq_put(txn, at_limit, val, YQ_PUT_UPSERT), YQ_OK);
    CHECK_RC(yq_put(txn, over_limit, val, YQ_PUT_UPSERT), YQ_ERR_TOOBIG);
    CHECK_RC(yq_del(txn, over_limit), YQ_ERR_TOOBIG);

    /* The 1024-byte key really is there, and the rejected one is not. */
    yq_slice got = { 0 };
    CHECK_RC(yq_get(txn, at_limit, &got), YQ_OK);
    CHECK(got.size == 1 && ((const char *)got.data)[0] == 'v');
    CHECK_RC(yq_get(txn, over_limit, &got), YQ_ERR_NOTFOUND);

    CHECK_RC(yq_txn_commit(txn), YQ_OK);
    CHECK(count_keys(db) == 1);
    yq_close(db);
    printf("%s\n", g_fail == before ? "PASS" : "FAIL");
}

/* ── oversized value: TOOBIG without touching the allocation ─────────────── */

static void test_value_size_limit(void) {
    printf("test_value_size_limit... ");
    int before = g_fail;
    yq_db *db = open_rw();
    if (!db) return;

    yq_txn *txn = NULL;
    CHECK_RC(yq_txn_begin(db, YQ_TXN_READWRITE, &txn), YQ_OK);

    /* 1 GiB + 1: the slice is never dereferenced, the check runs first, so a
     * bogus pointer keeps this test fast and allocation-free. */
    yq_slice too_big = { (const void *)"x", (size_t)(1ULL << 30) + 1 };
    yq_slice key = { "k", 1 };
    CHECK_RC(yq_put(txn, key, too_big, YQ_PUT_UPSERT), YQ_ERR_TOOBIG);

    /* Exactly 1 GiB is allowed by the contract, but allocating it here would
     * make the test 1 GiB of RSS for no extra coverage, so stop at the boundary
     * case that the check actually rejects. */
    yq_slice zero = { NULL, 0 };
    CHECK_RC(yq_put(txn, key, zero, YQ_PUT_UPSERT), YQ_OK); /* empty value is legal */

    CHECK_RC(yq_txn_commit(txn), YQ_OK);
    yq_close(db);
    printf("%s\n", g_fail == before ? "PASS" : "FAIL");
}

/* ── mode is a closed enum ───────────────────────────────────────────────── */

static void test_unknown_mode_is_inval(void) {
    printf("test_unknown_mode_is_inval... ");
    int before = g_fail;
    yq_db *db = open_rw();
    if (!db) return;

    yq_txn *txn = NULL;
    CHECK_RC(yq_txn_begin(db, YQ_TXN_READWRITE, &txn), YQ_OK);

    yq_slice key = { "mode", 4 };
    yq_slice val = { "first", 5 };
    CHECK_RC(yq_put(txn, key, val, YQ_PUT_UPSERT), YQ_OK);

    /* Every undefined bit pattern must be rejected rather than falling back to
     * UPSERT -- that fallback is what let a typo silently overwrite data. */
    const uint32_t bad_modes[] = { 2u, 3u, 0x10u, 0x80000000u, 0xFFFFFFFFu };
    yq_slice overwrite = { "second", 6 };
    for (size_t i = 0; i < sizeof bad_modes / sizeof bad_modes[0]; i++) {
        CHECK_RC(yq_put(txn, key, overwrite, bad_modes[i]), YQ_ERR_INVAL);
    }

    /* The value must still be the original one. */
    yq_slice got = { 0 };
    CHECK_RC(yq_get(txn, key, &got), YQ_OK);
    CHECK(got.size == 5 && memcmp(got.data, "first", 5) == 0);

    /* A rejected call must not poison the transaction. */
    CHECK_RC(yq_put(txn, key, overwrite, YQ_PUT_UPSERT), YQ_OK);
    CHECK_RC(yq_txn_commit(txn), YQ_OK);

    yq_txn *ro = NULL;
    CHECK_RC(yq_txn_begin(db, YQ_TXN_READONLY, &ro), YQ_OK);
    CHECK_RC(yq_get(ro, key, &got), YQ_OK);
    CHECK(got.size == 6 && memcmp(got.data, "second", 6) == 0);
    yq_txn_commit(ro);
    yq_close(db);
    printf("%s\n", g_fail == before ? "PASS" : "FAIL");
}

/* ── the batch API keeps its documented all-or-nothing INVAL ─────────────── */

static void test_batch_contract_unchanged(void) {
    printf("test_batch_contract_unchanged... ");
    int before = g_fail;
    yq_db *db = open_rw();
    if (!db) return;

    yq_txn *txn = NULL;
    CHECK_RC(yq_txn_begin(db, YQ_TXN_READWRITE, &txn), YQ_OK);

    static char buf[1025];
    memset(buf, 'b', sizeof buf);

    yq_slice ok_key = { "good", 4 };
    yq_slice ok_val = { "v", 1 };
    yq_slice bad_key = { buf, 1025 };
    yq_batch_entry entries[2];
    memset(entries, 0, sizeof entries);
    entries[0].key = bad_key;
    entries[0].val = ok_val;
    entries[1].key = ok_key;
    entries[1].val = ok_val;

    yq_batch_result res;
    memset(&res, 0, sizeof res);
    res.struct_size = sizeof res;

    /* The header documents "if any entry is invalid, nothing is applied and the
     * return value is YQ_ERR_INVAL" -- a whole-batch verdict, not per-entry
     * codes, and that is unchanged by this PR. */
    CHECK_RC(yq_batch_put(txn, entries, 2, &res), YQ_ERR_INVAL);
    CHECK(res.entries_total == 2);
    CHECK(res.entries_ok == 0);
    CHECK(res.entries_failed == 2);
    CHECK(res.first_error == YQ_ERR_INVAL);

    /* Nothing applied, including the valid entry. */
    yq_slice got = { 0 };
    CHECK_RC(yq_get(txn, ok_key, &got), YQ_ERR_NOTFOUND);

    /* A batch entry whose key has no storage is rejected the same way, and the
     * counters still add up. */
    yq_batch_entry null_entry;
    memset(&null_entry, 0, sizeof null_entry);
    null_entry.key.data = NULL;
    null_entry.key.size = 3;
    null_entry.val = ok_val;
    memset(&res, 0, sizeof res);
    res.struct_size = sizeof res;
    CHECK_RC(yq_batch_put(txn, &null_entry, 1, &res), YQ_ERR_INVAL);
    CHECK(res.entries_total == 1 && res.entries_ok == 0 && res.entries_failed == 1);

    yq_slice keys[1] = { { buf, 1025 } };
    memset(&res, 0, sizeof res);
    res.struct_size = sizeof res;
    CHECK_RC(yq_batch_del(txn, keys, 1, &res), YQ_ERR_INVAL);
    CHECK(res.entries_ok == 0 && res.entries_failed == 1);

    CHECK_RC(yq_txn_commit(txn), YQ_OK);
    CHECK(count_keys(db) == 0);
    yq_close(db);
    printf("%s\n", g_fail == before ? "PASS" : "FAIL");
}

/* ── the message for TOOBIG has to name both limits ──────────────────────── */

static void test_toobig_message_covers_keys(void) {
    printf("test_toobig_message_covers_keys... ");
    int before = g_fail;
    const char *msg = yq_strerror(YQ_ERR_TOOBIG);
    CHECK(msg != NULL);
    /* It used to read "value too big", which is actively wrong when a 1025-byte
     * *key* is what got rejected. */
    CHECK(strstr(msg, "key") != NULL);
    CHECK(strstr(msg, "value") != NULL);
    printf("%s (\"%s\")\n", g_fail == before ? "PASS" : "FAIL", msg);
}

int main(void) {
    test_empty_key_is_inval();
    test_key_size_limit();
    test_value_size_limit();
    test_unknown_mode_is_inval();
    test_batch_contract_unchanged();
    test_toobig_message_covers_keys();

    remove_db_files();

    if (g_fail) {
        printf("\nputargs_test: %d check(s) FAILED\n", g_fail);
        return 1;
    }
    printf("\nputargs_test: all argument-validation checks passed\n");
    return 0;
}
