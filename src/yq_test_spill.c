/*
 * P3-S2: the tree is durable, and everything that breaks when it starts
 * holding pages.
 *
 * Until a checkpoint writes it, the tree was two meta pages in the database
 * file and every byte of data lived in the log plus the memtable. Making the
 * checkpoint build it exposes a set of assumptions that were invisible while
 * the tree was empty, and each of these is one of them:
 *
 *   - the cursors concatenated the two sources instead of merging them, so a
 *     scan returned every key twice once the tree and the memtable both held
 *     it (test_scan_not_doubled_after_checkpoint);
 *   - a reopen could not read the tree's pages at all, because the store only
 *     resolved pages it had already read into memory
 *     (test_db_file_self_containing);
 *   - a checkpoint after a reopen that had no log would rebuild the tree from
 *     a memtable that held only the newest writes, erasing everything else
 *     (test_checkpoint_after_logless_reopen_keeps_tree).
 *
 * The last one is the reason yq_db.mt_complete exists.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "yq.h"

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

#define DB   "spill_db"
#define LOG  "spill_db.log"
#define LOCK "spill_db.lock"
#define SHM  "spill_db.shm"

static void rm_all(void) {
    remove(DB); remove(LOG); remove(LOCK); remove(SHM);
}

static long file_len(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

static void opts_of(yq_opts *o, uint64_t memtable_bytes) {
    memset(o, 0, sizeof(*o));
    o->struct_size = sizeof(yq_opts);
    o->flags = YQ_OPEN_CREATE;
    o->page_size = 4096;
    o->memtable_bytes = memtable_bytes;
    o->log_bytes = 1024ull * 1024 * 1024;   /* keep auto-checkpoint out of it */
}

static void put_keys(yq_db *db, const char *tag, int from, int to) {
    uint8_t val[64];
    memset(val, 'v', sizeof(val));
    yq_txn *t = NULL;
    CHECK(yq_txn_begin(db, YQ_TXN_READWRITE, &t) == YQ_OK);
    for (int i = from; i < to; i++) {
        char k[32];
        snprintf(k, sizeof(k), "%s%06d", tag, i);
        yq_slice key = {k, strlen(k)}, v = {val, sizeof(val)};
        CHECK(yq_put(t, key, v, YQ_PUT_UPSERT) == YQ_OK);
    }
    CHECK(yq_txn_commit(t) == YQ_OK);
}

static int count_missing(yq_db *db, const char *tag, int from, int to) {
    int miss = 0;
    yq_txn *t = NULL;
    CHECK(yq_txn_begin(db, YQ_TXN_READONLY, &t) == YQ_OK);
    for (int i = from; i < to; i++) {
        char k[32];
        snprintf(k, sizeof(k), "%s%06d", tag, i);
        yq_slice key = {k, strlen(k)}, out = {0};
        if (yq_get(t, key, &out) != YQ_OK) miss++;
        else if (out.size != 64) miss++;
    }
    CHECK(yq_txn_commit(t) == YQ_OK);
    return miss;
}

static int scan_count(yq_db *db) {
    yq_txn *t = NULL;
    CHECK(yq_txn_begin(db, YQ_TXN_READONLY, &t) == YQ_OK);
    yq_cur *c = NULL;
    CHECK(yq_cur_open(t, &c) == YQ_OK);
    int n = 0;
    for (int rc = yq_cur_first(c); rc == YQ_OK; rc = yq_cur_next(c)) n++;
    yq_cur_close(c);
    CHECK(yq_txn_commit(t) == YQ_OK);
    return n;
}

/* ── the tree actually gets built and persisted ───────────────────────── */

static void test_checkpoint_builds_tree(void) {
    printf("test_checkpoint_builds_tree... ");
    rm_all();

    yq_opts o;
    opts_of(&o, 64u * 1024 * 1024);
    yq_db *db = NULL;
    CHECK(yq_open(DB, &o, &db) == YQ_OK);

    yq_stat st;
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK(yq_db_stat(db, &st) == YQ_OK);
    CHECK(st.npages == 2);                    /* meta pages only */
    CHECK(file_len(DB) == 2 * 4096);

    put_keys(db, "k", 0, 20000);
    CHECK(yq_db_stat(db, &st) == YQ_OK);
    CHECK(st.npages == 2);                    /* still memtable only */

    CHECK(yq_checkpoint(db) == YQ_OK);
    CHECK(yq_db_stat(db, &st) == YQ_OK);
    CHECK(st.npages > 100);                   /* a real tree now */
    CHECK(st.free_pages <= 2);                /* LIFO free list is empty here */
    CHECK(file_len(DB) >= (long)st.npages * 4096);

    /* The data is in the tree, not only in the log. */
    CHECK(count_missing(db, "k", 0, 20000) == 0);
    CHECK(scan_count(db) == 20000);

    yq_close(db);
    rm_all();
    printf("OK\n");
}

/* ── the merge: a scan must not emit the tree's copy of a key ─────────── */

static void test_scan_not_doubled_after_checkpoint(void) {
    printf("test_scan_not_doubled_after_checkpoint... ");
    rm_all();

    yq_opts o;
    opts_of(&o, 64u * 1024 * 1024);
    yq_db *db = NULL;
    CHECK(yq_open(DB, &o, &db) == YQ_OK);

    put_keys(db, "k", 0, 200);
    CHECK(yq_checkpoint(db) == YQ_OK);
    /* Both sources hold all 200 keys now: the tree as a copy, the memtable as
     * the live set. Concatenating them was the old behaviour. */
    CHECK(scan_count(db) == 200);

    /* Walking backwards must agree with walking forwards. */
    yq_txn *t = NULL;
    CHECK(yq_txn_begin(db, YQ_TXN_READONLY, &t) == YQ_OK);
    yq_cur *c = NULL;
    CHECK(yq_cur_open(t, &c) == YQ_OK);
    int fwd = 0, back = 0;
    for (int rc = yq_cur_first(c); rc == YQ_OK; rc = yq_cur_next(c)) {
        yq_slice k = {0};
        CHECK(yq_cur_key(c, &k) == YQ_OK);
        fwd++;
        /* and the value must come from the memtable, not the tree's copy */
        yq_slice v = {0};
        CHECK(yq_cur_val(c, &v) == YQ_OK);
        CHECK(v.size == 64);
    }
    CHECK(fwd == 200);
    for (int rc = yq_cur_last(c); rc == YQ_OK; rc = yq_cur_prev(c)) back++;
    CHECK(back == 200);
    yq_cur_close(c);
    CHECK(yq_txn_commit(t) == YQ_OK);

    /* seek() positions into the merged order, not into one source. */
    CHECK(yq_txn_begin(db, YQ_TXN_READONLY, &t) == YQ_OK);
    CHECK(yq_cur_open(t, &c) == YQ_OK);
    yq_slice target = {"k000100", 7};
    CHECK(yq_cur_seek(c, target) == YQ_OK);
    yq_slice k = {0};
    CHECK(yq_cur_key(c, &k) == YQ_OK);
    CHECK(k.size == 7 && memcmp(k.data, "k000100", 7) == 0);
    int n = 0;
    for (int rc = YQ_OK; rc == YQ_OK; rc = yq_cur_next(c)) n++;
    CHECK(n == 100);                          /* 200 - 100 already emitted */
    yq_cur_close(c);
    CHECK(yq_txn_commit(t) == YQ_OK);

    yq_close(db);
    rm_all();
    printf("OK\n");
}

/* ── the database file alone is a complete database ───────────────────── */

static void test_db_file_self_containing(void) {
    printf("test_db_file_self_containing... ");
    rm_all();

    yq_opts o;
    opts_of(&o, 64u * 1024 * 1024);
    yq_db *db = NULL;
    CHECK(yq_open(DB, &o, &db) == YQ_OK);
    put_keys(db, "k", 0, 20000);
    CHECK(yq_checkpoint(db) == YQ_OK);
    yq_close(db);

    /* The log was the only other copy of the dataset. Remove it. */
    CHECK(remove(LOG) == 0);

    yq_db *db2 = NULL;
    CHECK(yq_open(DB, &o, &db2) == YQ_OK);
    yq_stat st;
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK(yq_db_stat(db2, &st) == YQ_OK);
    CHECK(st.npages > 100);                   /* restored from the file */
    CHECK(st.memtable_bytes == 0);            /* nothing was replayed */
    CHECK(st.log_bytes == 0);

    CHECK(count_missing(db2, "k", 0, 20000) == 0);
    CHECK(scan_count(db2) == 20000);

    /* a key that was never written still misses */
    yq_txn *t = NULL;
    CHECK(yq_txn_begin(db2, YQ_TXN_READONLY, &t) == YQ_OK);
    yq_slice nope = {"nosuchkey", 9}, out = {0};
    CHECK(yq_get(t, nope, &out) == YQ_ERR_NOTFOUND);
    CHECK(yq_txn_commit(t) == YQ_OK);

    yq_close(db2);
    rm_all();
    printf("OK\n");
}

/* ── the regression: a checkpoint must not erase the tree ─────────────── */

static void test_checkpoint_after_logless_reopen_keeps_tree(void) {
    printf("test_checkpoint_after_logless_reopen_keeps_tree... ");
    rm_all();

    yq_opts o;
    opts_of(&o, 64u * 1024 * 1024);
    yq_db *db = NULL;
    CHECK(yq_open(DB, &o, &db) == YQ_OK);
    put_keys(db, "old", 0, 5000);
    CHECK(yq_checkpoint(db) == YQ_OK);
    yq_close(db);

    CHECK(remove(LOG) == 0);                   /* the tree is now the only copy */

    /* Reopen with no log: the memtable is empty and the tree holds it all. */
    yq_db *db2 = NULL;
    CHECK(yq_open(DB, &o, &db2) == YQ_OK);
    yq_stat st;
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK(yq_db_stat(db2, &st) == YQ_OK);
    CHECK(st.npages > 100);
    CHECK(count_missing(db2, "old", 0, 5000) == 0);

    /*
     * New writes land in that empty memtable, so the memtable now holds only
     * the newest keys. A checkpoint that rebuilt the tree from it would reset
     * every page and allocate only the new ones: the old keys would be gone
     * from the tree, and the log is gone, so they would be gone everywhere.
     */
    put_keys(db2, "new", 0, 100);
    CHECK(yq_checkpoint(db2) == YQ_OK);

    /* Still there, before the close? */
    CHECK(count_missing(db2, "old", 0, 5000) == 0);
    CHECK(count_missing(db2, "new", 0, 100) == 0);

    yq_close(db2);

    /* And after a reopen, still with no log. */
    yq_db *db3 = NULL;
    CHECK(yq_open(DB, &o, &db3) == YQ_OK);
    CHECK(count_missing(db3, "old", 0, 5000) == 0);
    CHECK(count_missing(db3, "new", 0, 100) == 0);
    /* The scan sees both, once each. */
    CHECK(scan_count(db3) == 5100);
    yq_close(db3);

    rm_all();
    printf("OK\n");
}

/* ── the memtable is the newer source ─────────────────────────────────── */

static void test_memtable_wins_over_tree(void) {
    printf("test_memtable_wins_over_tree... ");
    rm_all();

    yq_opts o;
    opts_of(&o, 64u * 1024 * 1024);
    yq_db *db = NULL;
    CHECK(yq_open(DB, &o, &db) == YQ_OK);

    {
        yq_txn *t = NULL;
        CHECK(yq_txn_begin(db, YQ_TXN_READWRITE, &t) == YQ_OK);
        yq_slice k = {"dup", 3}, v = {"first", 5};
        CHECK(yq_put(t, k, v, YQ_PUT_UPSERT) == YQ_OK);
        CHECK(yq_txn_commit(t) == YQ_OK);
    }
    CHECK(yq_checkpoint(db) == YQ_OK);         /* the tree holds "first" */

    {
        yq_txn *t = NULL;
        CHECK(yq_txn_begin(db, YQ_TXN_READWRITE, &t) == YQ_OK);
        yq_slice k = {"dup", 3}, v = {"second", 6};
        CHECK(yq_put(t, k, v, YQ_PUT_UPSERT) == YQ_OK);
        CHECK(yq_txn_commit(t) == YQ_OK);
    }

    yq_txn *rt = NULL;
    CHECK(yq_txn_begin(db, YQ_TXN_READONLY, &rt) == YQ_OK);
    yq_slice k = {"dup", 3}, out = {0};
    CHECK(yq_get(rt, k, &out) == YQ_OK);
    CHECK(out.size == 6 && memcmp(out.data, "second", 6) == 0);
    CHECK(yq_txn_commit(rt) == YQ_OK);

    /* one row, not two, and it is the newer value */
    yq_cur *c = NULL;
    CHECK(yq_txn_begin(db, YQ_TXN_READONLY, &rt) == YQ_OK);
    CHECK(yq_cur_open(rt, &c) == YQ_OK);
    int n = 0;
    for (int rc = yq_cur_first(c); rc == YQ_OK; rc = yq_cur_next(c)) {
        yq_slice ck = {0}, cv = {0};
        CHECK(yq_cur_key(c, &ck) == YQ_OK);
        CHECK(ck.size == 3 && memcmp(ck.data, "dup", 3) == 0);
        CHECK(yq_cur_val(c, &cv) == YQ_OK);
        CHECK(cv.size == 6 && memcmp(cv.data, "second", 6) == 0);
        n++;
    }
    CHECK(n == 1);
    yq_cur_close(c);
    CHECK(yq_txn_commit(rt) == YQ_OK);

    /* delete after the checkpoint: the tree's copy must not resurrect it */
    {
        yq_txn *t = NULL;
        CHECK(yq_txn_begin(db, YQ_TXN_READWRITE, &t) == YQ_OK);
        CHECK(yq_del(t, k) == YQ_OK);
        CHECK(yq_txn_commit(t) == YQ_OK);
    }
    CHECK(yq_txn_begin(db, YQ_TXN_READONLY, &rt) == YQ_OK);
    CHECK(yq_get(rt, k, &out) == YQ_ERR_NOTFOUND);
    CHECK(yq_txn_commit(rt) == YQ_OK);

    yq_close(db);
    rm_all();
    printf("OK\n");
}

/* ── a delete must not come back from the tree ────────────────────────── */

static void test_delete_survives_checkpoint_without_log(void) {
    printf("test_delete_survives_checkpoint_without_log... ");
    rm_all();

    yq_opts o;
    opts_of(&o, 64u * 1024 * 1024);
    yq_db *db = NULL;
    CHECK(yq_open(DB, &o, &db) == YQ_OK);
    put_keys(db, "k", 0, 3000);

    {
        yq_txn *t = NULL;
        CHECK(yq_txn_begin(db, YQ_TXN_READWRITE, &t) == YQ_OK);
        for (int i = 0; i < 3000; i += 2) {          /* every other key */
            char kbuf[32];
            snprintf(kbuf, sizeof(kbuf), "k%06d", i);
            yq_slice key = {kbuf, strlen(kbuf)};
            CHECK(yq_del(t, key) == YQ_OK);
        }
        CHECK(yq_txn_commit(t) == YQ_OK);
    }
    CHECK(yq_checkpoint(db) == YQ_OK);
    yq_close(db);

    CHECK(remove(LOG) == 0);

    yq_db *db2 = NULL;
    CHECK(yq_open(DB, &o, &db2) == YQ_OK);
    CHECK(scan_count(db2) == 1500);           /* the deleted half is gone */
    int miss = 0;
    yq_txn *t = NULL;
    CHECK(yq_txn_begin(db2, YQ_TXN_READONLY, &t) == YQ_OK);
    for (int i = 1; i < 3000; i += 2) {
        char kbuf[32];
        snprintf(kbuf, sizeof(kbuf), "k%06d", i);
        yq_slice key = {kbuf, strlen(kbuf)}, out = {0};
        if (yq_get(t, key, &out) != YQ_OK) miss++;
    }
    CHECK(yq_txn_commit(t) == YQ_OK);
    CHECK(miss == 0);

    yq_close(db2);
    rm_all();
    printf("OK\n");
}

/* ── repeated checkpoints must reuse pages, not grow the file ─────────── */

static void test_repeated_checkpoints_reuse_pages(void) {
    printf("test_repeated_checkpoints_reuse_pages... ");
    rm_all();

    yq_opts o;
    opts_of(&o, 64u * 1024 * 1024);
    yq_db *db = NULL;
    CHECK(yq_open(DB, &o, &db) == YQ_OK);
    put_keys(db, "k", 0, 8000);
    CHECK(yq_checkpoint(db) == YQ_OK);

    yq_stat st;
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK(yq_db_stat(db, &st) == YQ_OK);
    uint64_t npages = st.npages;
    long len = file_len(DB);
    CHECK(npages > 100);

    /* Four more checkpoints over the same live set: each one resets the store
     * and rebuilds, so a leak in the reset would show up as a growing file. */
    for (int i = 0; i < 4; i++) {
        CHECK(yq_checkpoint(db) == YQ_OK);
        CHECK(yq_db_stat(db, &st) == YQ_OK);
        CHECK(st.npages == npages);
        CHECK(file_len(DB) == len);
    }

    /* And writes after that still land in the same page budget. */
    put_keys(db, "k", 8000, 9000);
    CHECK(yq_checkpoint(db) == YQ_OK);
    CHECK(yq_db_stat(db, &st) == YQ_OK);
    CHECK(st.npages > npages);                /* it grew, once, for the new data */
    CHECK(count_missing(db, "k", 0, 9000) == 0);

    yq_close(db);
    rm_all();
    printf("OK\n");
}

/* ── the resident page set is bounded ─────────────────────────────────── */

static void test_evicted_pages_read_back(void) {
    printf("test_evicted_pages_read_back... ");
    rm_all();

    /*
     * A checkpoint leaves one buffer per tree page resident, which for a big
     * tree is a second copy of the dataset on top of the memtable. The
     * checkpoint therefore releases clean pages past a cap (a quarter of
     * memtable_bytes, floor 256 pages) -- they are on disk, so a later lookup
     * has to read them back.
     *
     * A small memtable makes the cap bite: 20000 keys x ~170 B is ~3200 pages
     * against a 256-page cap.
     */
    yq_opts o;
    opts_of(&o, 4u * 1024 * 1024);
    yq_db *db = NULL;
    CHECK(yq_open(DB, &o, &db) == YQ_OK);
    put_keys(db, "k", 0, 20000);

    CHECK(yq_checkpoint(db) == YQ_OK);

    yq_stat st;
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK(yq_db_stat(db, &st) == YQ_OK);
    CHECK(st.npages > 256);                   /* more pages than the cap */

    yq_close(db);
    CHECK(remove(LOG) == 0);

    /* Nothing is in memory any more, so every lookup walks down through pages
     * that must be pread back out of the file. */
    yq_db *db2 = NULL;
    CHECK(yq_open(DB, &o, &db2) == YQ_OK);
    CHECK(count_missing(db2, "k", 0, 20000) == 0);
    CHECK(scan_count(db2) == 20000);
    yq_close(db2);

    rm_all();
    printf("OK\n");
}

int main(void) {
    printf("=== yq-DB B+Tree Spill Tests (P3-S2) ===\n\n");

    test_checkpoint_builds_tree();
    test_scan_not_doubled_after_checkpoint();
    test_db_file_self_containing();
    test_checkpoint_after_logless_reopen_keeps_tree();
    test_memtable_wins_over_tree();
    test_delete_survives_checkpoint_without_log();
    test_repeated_checkpoints_reuse_pages();
    test_evicted_pages_read_back();

    printf("\n=== ALL TESTS PASSED ===\n");
    return 0;
}
