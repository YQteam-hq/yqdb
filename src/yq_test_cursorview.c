/*
 * yq_test_cursorview.c -- cursor visibility and lifetime.
 *
 * Two things about cursors were undocumented and one of them was unimplemented:
 *
 * 1. Visibility. yq_get() is read-your-writes: inside a read-write transaction
 *    it returns keys that transaction just put, and reports a key it just
 *    deleted as missing. A cursor over the *same* transaction shows neither --
 *    it walks committed data. Both APIs read at read-committed level, so a
 *    commit made by another transaction after yours began is visible to later
 *    lookups. The header used to claim "MVCC snapshot reads" / a stable
 *    snapshot, which the read path does not implement (snapshot_root is
 *    registered so a checkpoint cannot recycle pages under a reader, not to pin
 *    visibility). The docs now say read committed, and this test pins it so the
 *    wording cannot drift away from the code again.
 *
 * 2. Lifetime. The header has always promised that "an unclosed cursor is
 *    invalidated when its transaction ends", but yq_txn_commit()/yq_txn_abort()
 *    simply freed the transaction: yq_cur_valid() kept reporting a positioned
 *    cursor, and yq_cur_seek() kept serving rows, for a transaction that no
 *    longer existed. Every cursor is now registered with its transaction and
 *    detached on commit/abort, so a dead cursor reports YQ_ERR_CURSOR (the code
 *    ERRORS.md defines for "游标所属事务已结束") instead of stale data.
 */

#include <stdio.h>
#include <string.h>

#include "yq.h"

static const char *TEST_DB = "yqtest_cursorview.yqdb";

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

static void seed(yq_db *db, const char *k, const char *v) {
    yq_txn *txn = NULL;
    if (yq_txn_begin(db, YQ_TXN_READWRITE, &txn) != YQ_OK) return;
    yq_put(txn, (yq_slice){ k, strlen(k) }, (yq_slice){ v, strlen(v) },
           YQ_PUT_UPSERT);
    yq_txn_commit(txn);
}

/* Walk the cursor and report whether `key` shows up. */
static int cursor_has(yq_cur *cur, const char *key) {
    size_t n = strlen(key);
    for (int r = yq_cur_first(cur); r == YQ_OK; r = yq_cur_next(cur)) {
        yq_slice k = { 0 };
        if (yq_cur_key(cur, &k) != YQ_OK) continue;
        if (k.size == n && memcmp(k.data, key, n) == 0) return 1;
    }
    return 0;
}

/* ── get() is read-your-writes; the cursor shows committed data ──────────── */

static void test_get_sees_own_writes_cursor_does_not(void) {
    printf("test_get_sees_own_writes_cursor_does_not... ");
    int before = g_fail;
    yq_db *db = open_rw();
    if (!db) {
        printf("FAIL (fixture)\n");
        g_fail++;
        return;
    }

    seed(db, "a", "1");
    seed(db, "b", "2");

    yq_txn *txn = NULL;
    CHECK(yq_txn_begin(db, YQ_TXN_READWRITE, &txn) == YQ_OK);
    CHECK(yq_put(txn, (yq_slice){ "c", 1 }, (yq_slice){ "3", 1 }, YQ_PUT_UPSERT) == YQ_OK);
    CHECK(yq_del(txn, (yq_slice){ "a", 1 }) == YQ_OK);

    /* yq_get(): this transaction sees its own writes and its own deletion. */
    yq_slice got = { 0 };
    CHECK(yq_get(txn, (yq_slice){ "c", 1 }, &got) == YQ_OK);
    CHECK(got.size == 1 && ((const char *)got.data)[0] == '3');
    CHECK(yq_get(txn, (yq_slice){ "a", 1 }, &got) == YQ_ERR_NOTFOUND);

    /* A cursor bound to the same transaction sees committed data instead. */
    yq_cur *cur = NULL;
    CHECK(yq_cur_open(txn, &cur) == YQ_OK);
    CHECK(cursor_has(cur, "a") == 1); /* deleted, but still committed here */
    CHECK(cursor_has(cur, "b") == 1);
    CHECK(cursor_has(cur, "c") == 0); /* not committed yet */

    yq_cur_close(cur);
    CHECK(yq_txn_commit(txn) == YQ_OK);

    /* After commit both APIs agree, and the deletion is now visible. */
    yq_txn *after = NULL;
    CHECK(yq_txn_begin(db, YQ_TXN_READONLY, &after) == YQ_OK);
    yq_cur *cur2 = NULL;
    CHECK(yq_cur_open(after, &cur2) == YQ_OK);
    CHECK(cursor_has(cur2, "a") == 0);
    CHECK(cursor_has(cur2, "b") == 1);
    CHECK(cursor_has(cur2, "c") == 1);
    yq_cur_close(cur2);
    yq_txn_commit(after);

    yq_close(db);
    printf("%s\n", g_fail == before ? "PASS" : "FAIL");
}

/* ── isolation level: read committed, not repeatable read ────────────────── */

static void test_reads_are_read_committed(void) {
    printf("test_reads_are_read_committed... ");
    int before = g_fail;
    yq_db *db = open_rw();
    if (!db) {
        printf("FAIL (fixture)\n");
        g_fail++;
        return;
    }

    seed(db, "k", "v1");

    yq_txn *reader = NULL;
    CHECK(yq_txn_begin(db, YQ_TXN_READONLY, &reader) == YQ_OK);
    yq_slice got = { 0 };
    CHECK(yq_get(reader, (yq_slice){ "k", 1 }, &got) == YQ_OK);
    CHECK(got.size == 2 && memcmp(got.data, "v1", 2) == 0);

    /* Another transaction commits while the reader is open. */
    seed(db, "k", "v2");
    seed(db, "new", "x");

    /* Read committed: the reader's later lookups see the new commit. This is
     * the documented contract now -- it is explicitly *not* repeatable read. */
    CHECK(yq_get(reader, (yq_slice){ "k", 1 }, &got) == YQ_OK);
    CHECK(got.size == 2 && memcmp(got.data, "v2", 2) == 0);

    yq_cur *cur = NULL;
    CHECK(yq_cur_open(reader, &cur) == YQ_OK);
    CHECK(cursor_has(cur, "new") == 1); /* same contract as yq_get */
    yq_cur_close(cur);

    yq_txn_commit(reader);
    yq_close(db);
    printf("%s\n", g_fail == before ? "PASS" : "FAIL");
}

/* ── a cursor dies with its transaction instead of going stale ───────────── */

static void test_cursor_dies_with_its_transaction(void) {
    printf("test_cursor_dies_with_its_transaction... ");
    int before = g_fail;
    yq_db *db = open_rw();
    if (!db) {
        printf("FAIL (fixture)\n");
        g_fail++;
        return;
    }

    seed(db, "a", "1");

    yq_txn *txn = NULL;
    CHECK(yq_txn_begin(db, YQ_TXN_READONLY, &txn) == YQ_OK);
    yq_cur *cur = NULL;
    CHECK(yq_cur_open(txn, &cur) == YQ_OK);
    CHECK(yq_cur_first(cur) == YQ_OK);
    CHECK(yq_cur_valid(cur) != 0);

    CHECK(yq_txn_commit(txn) == YQ_OK);

    /* Before the fix: valid() still said 1 and seek() still returned rows. */
    yq_slice k = { 0 };
    CHECK(yq_cur_valid(cur) == 0);
    CHECK(yq_cur_first(cur) == YQ_ERR_CURSOR);
    CHECK(yq_cur_last(cur) == YQ_ERR_CURSOR);
    CHECK(yq_cur_next(cur) == YQ_ERR_CURSOR);
    CHECK(yq_cur_prev(cur) == YQ_ERR_CURSOR);
    CHECK(yq_cur_seek(cur, (yq_slice){ "a", 1 }) == YQ_ERR_CURSOR);
    CHECK(yq_cur_seek_exact(cur, (yq_slice){ "a", 1 }) == YQ_ERR_CURSOR);
    CHECK(yq_cur_key(cur, &k) == YQ_ERR_CURSOR);

    /* Closing a dead cursor is still required, and still fine. */
    CHECK(yq_cur_close(cur) == YQ_OK);

    /* Abort must detach too, not only commit. */
    yq_txn *txn2 = NULL;
    CHECK(yq_txn_begin(db, YQ_TXN_READWRITE, &txn2) == YQ_OK);
    yq_cur *cur2 = NULL;
    CHECK(yq_cur_open(txn2, &cur2) == YQ_OK);
    CHECK(yq_cur_first(cur2) == YQ_OK);
    CHECK(yq_txn_abort(txn2) == YQ_OK);
    CHECK(yq_cur_valid(cur2) == 0);
    CHECK(yq_cur_next(cur2) == YQ_ERR_CURSOR);
    CHECK(yq_cur_close(cur2) == YQ_OK);

    yq_close(db);
    printf("%s\n", g_fail == before ? "PASS" : "FAIL");
}

/* ── several cursors per transaction, closed in mixed order ──────────────── */

static void test_many_cursors_per_transaction(void) {
    printf("test_many_cursors_per_transaction... ");
    int before = g_fail;
    yq_db *db = open_rw();
    if (!db) {
        printf("FAIL (fixture)\n");
        g_fail++;
        return;
    }

    seed(db, "a", "1");
    seed(db, "b", "2");

    yq_txn *txn = NULL;
    CHECK(yq_txn_begin(db, YQ_TXN_READONLY, &txn) == YQ_OK);

    yq_cur *c1 = NULL, *c2 = NULL, *c3 = NULL;
    CHECK(yq_cur_open(txn, &c1) == YQ_OK);
    CHECK(yq_cur_open(txn, &c2) == YQ_OK);
    CHECK(yq_cur_open(txn, &c3) == YQ_OK);
    CHECK(yq_cur_first(c1) == YQ_OK);
    CHECK(yq_cur_first(c2) == YQ_OK);
    CHECK(yq_cur_first(c3) == YQ_OK);

    /* Close in the middle of the list first, so the unlink has to stitch
     * neighbours rather than just drop the head. */
    CHECK(yq_cur_close(c2) == YQ_OK);

    CHECK(yq_txn_commit(txn) == YQ_OK);

    /* The survivor and the head are both detaching candidates. */
    CHECK(yq_cur_valid(c1) == 0);
    CHECK(yq_cur_valid(c3) == 0);
    CHECK(yq_cur_next(c1) == YQ_ERR_CURSOR);
    CHECK(yq_cur_next(c3) == YQ_ERR_CURSOR);
    CHECK(yq_cur_close(c1) == YQ_OK);
    CHECK(yq_cur_close(c3) == YQ_OK);

    yq_close(db);
    printf("%s\n", g_fail == before ? "PASS" : "FAIL");
}

int main(void) {
    test_get_sees_own_writes_cursor_does_not();
    test_reads_are_read_committed();
    test_cursor_dies_with_its_transaction();
    test_many_cursors_per_transaction();

    remove_db_files();

    if (g_fail) {
        printf("\ncursorview_test: %d check(s) FAILED\n", g_fail);
        return 1;
    }
    printf("\ncursorview_test: cursor visibility and lifetime match the header\n");
    return 0;
}
