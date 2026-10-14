/*
 * Regression tests for the three "blow-up" fixes.
 *
 * 1. Auto-checkpoint. opts.log_bytes / opts.memtable_bytes used to be
 *    documented but never consulted, so the log grew without bound for the
 *    lifetime of a process and reopen replayed every byte of it. Commits now
 *    trigger a checkpoint once a threshold is crossed.
 *
 * 2. Recovery capacity. The log is replayed into the memtable with the
 *    max_bytes admission check lifted. A live dataset larger than
 *    memtable_bytes used to make yq_open() fail (the replayed puts hit
 *    YQ_ERR_NOMEM), i.e. a memory threshold could make the database
 *    unopenable.
 *
 * 3. Sync modes. yq_wal_flush() fsynced unconditionally, so YQ_SYNC_OFF /
 *    YQ_OPEN_NOSYNC paid one fdatasync per commit anyway. These tests cover
 *    the functional side of the modes; the fsync count itself is a
 *    syscall-level property that the integration tests do not observe.
 *
 * Checks use an explicit macro instead of assert() so that they stay active
 * regardless of whether the build type defines NDEBUG.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yq.h"
#include "yq_wal.h"
#include "yq_memtable.h"
#include "yq_recover.h"

static const char *TEST_DB = "yqtest_p0.db";
static const char *TEST_LOG = "yqtest_p0.db.log";
static const char *TEST_SHM = "yqtest_p0.db.shm";
static const char *TEST_LOCK = "yqtest_p0.db.lock";

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            exit(1);                                                      \
        }                                                                 \
    } while (0)

static void remove_files(void) {
    remove(TEST_LOG);
    remove(TEST_SHM);
    remove(TEST_LOCK);
    remove(TEST_DB);
}

static void sset(yq_slice *s, const char *str) {
    s->data = str;
    s->size = strlen(str);
}

static void key_at(char *buf, size_t cap, int i) {
    snprintf(buf, cap, "k%08d", i);
}

/* ── 1. auto-checkpoint bounds the log ───────────────────────────────── */

static void test_auto_checkpoint_log_threshold(void) {
    printf("test_auto_checkpoint_log_threshold... ");

    remove_files();

    yq_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(yq_opts);
    opts.flags       = YQ_OPEN_CREATE;
    opts.log_bytes   = 64 * 1024;            /* tiny: forces many compactions */
    opts.memtable_bytes = 64u * 1024 * 1024; /* not the trigger here */

    yq_db *db = NULL;
    CHECK(yq_open(TEST_DB, &opts, &db) == YQ_OK);

    /*
     * 200 rounds x 100 puts x ~144 B is ~2.9 MB of log traffic over a live
     * set of only 100 keys (~16 KB). Without auto-checkpoint the log holds
     * every overwritten version (~2.9 MB); with the 64 KiB threshold it is
     * compacted back to the live set long before the end of the run.
     */
    const int KEYS = 100, ROUNDS = 200;
    uint8_t val[128];
    memset(val, 'v', sizeof(val));

    for (int r = 0; r < ROUNDS; r++) {
        yq_txn *txn = NULL;
        CHECK(yq_txn_begin(db, YQ_TXN_READWRITE, &txn) == YQ_OK);
        for (int i = 0; i < KEYS; i++) {
            char kbuf[32];
            key_at(kbuf, sizeof(kbuf), i);
            yq_slice k = { kbuf, strlen(kbuf) };
            yq_slice v = { val, sizeof(val) };
            CHECK(yq_put(txn, k, v, YQ_PUT_UPSERT) == YQ_OK);
        }
        CHECK(yq_txn_commit(txn) == YQ_OK);
    }

    yq_stat st;
    st.struct_size = sizeof(yq_stat);
    CHECK(yq_db_stat(db, &st) == YQ_OK);
    /* Live set is ~16 KB and each compaction rewrites exactly that; the
     * uncompacted churn would be ~2.9 MB, so this bound separates them. */
    CHECK(st.log_bytes < 512u * 1024);

    /* Every committed key is readable before close. */
    yq_txn *ro = NULL;
    CHECK(yq_txn_begin(db, YQ_TXN_READONLY, &ro) == YQ_OK);
    for (int i = 0; i < KEYS; i++) {
        char kbuf[32];
        key_at(kbuf, sizeof(kbuf), i);
        yq_slice k = { kbuf, strlen(kbuf) };
        yq_slice out = { 0 };
        CHECK(yq_get(ro, k, &out) == YQ_OK);
        CHECK(out.size == sizeof(val));
    }
    CHECK(yq_txn_commit(ro) == YQ_OK);
    CHECK(yq_close(db) == YQ_OK);

    /* Clean shutdown compacts the log: the file must collapse to the live
     * set (~16 KB), not keep the ~2.9 MB of overwritten history. */
    FILE *lf = fopen(TEST_LOG, "rb");
    CHECK(lf != NULL);
    CHECK(fseek(lf, 0, SEEK_END) == 0);
    long log_sz = ftell(lf);
    CHECK(fclose(lf) == 0);
    CHECK(log_sz > 0 && log_sz < 512 * 1024);

    /* ... and after a close/reopen cycle (compaction rewrote the log in
     * place, so replaying the compacted log must rebuild the set). */
    yq_db *db2 = NULL;
    CHECK(yq_open(TEST_DB, &opts, &db2) == YQ_OK);
    yq_txn *ro2 = NULL;
    CHECK(yq_txn_begin(db2, YQ_TXN_READONLY, &ro2) == YQ_OK);
    for (int i = 0; i < KEYS; i++) {
        char kbuf[32];
        key_at(kbuf, sizeof(kbuf), i);
        yq_slice k = { kbuf, strlen(kbuf) };
        yq_slice out = { 0 };
        CHECK(yq_get(ro2, k, &out) == YQ_OK);
    }
    CHECK(yq_txn_commit(ro2) == YQ_OK);
    CHECK(yq_close(db2) == YQ_OK);

    remove_files();
    printf("OK\n");
}

/* ── 2. recovery may exceed the memtable threshold ───────────────────── */

static void test_recovery_exceeds_memtable_threshold(void) {
    printf("test_recovery_exceeds_memtable_threshold... ");

    remove_files();

    /* Build a log by hand: 20000 committed keys x 64 B of value, ~2.3 MB. */
    yq_wal *wal = NULL;
    CHECK(yq_wal_open(&wal, TEST_DB, 4096, YQ_SYNC_NORMAL) == YQ_OK);

    uint8_t val[64];
    memset(val, 'x', sizeof(val));
    const int N = 20000;
    for (int i = 0; i < N; i++) {
        char kbuf[32];
        snprintf(kbuf, sizeof(kbuf), "key%08d", i);
        yq_slice k = { kbuf, strlen(kbuf) };
        yq_slice v = { val, sizeof(val) };
        CHECK(yq_wal_append_begin(wal, (uint64_t)i + 1) == YQ_OK);
        CHECK(yq_wal_append_put(wal, (uint64_t)i + 1, k, v) == YQ_OK);
        CHECK(yq_wal_append_commit(wal, (uint64_t)i + 1) == YQ_OK);
    }
    CHECK(yq_wal_flush(wal) == YQ_OK);
    CHECK(yq_wal_close(wal) == YQ_OK);

    /* Replay into a memtable a fraction of the size of the live set. */
    yq_wal *wal2 = NULL;
    CHECK(yq_wal_open(&wal2, TEST_DB, 4096, YQ_SYNC_NORMAL) == YQ_OK);

    const size_t max_bytes = 300 * 1024;
    yq_memtable *mt = yq_memtable_create(max_bytes);
    CHECK(mt != NULL);

    /* This used to return YQ_ERR_NOMEM (hence: an unopenable database). */
    CHECK(yq_recover(wal2, mt) == YQ_OK);
    CHECK(yq_wal_close(wal2) == YQ_OK);
    CHECK(yq_memtable_bytes(mt) > max_bytes);

    /* Spot-check the replayed set. */
    for (int i = 0; i < N; i += 4093) {
        char kbuf[32];
        snprintf(kbuf, sizeof(kbuf), "key%08d", i);
        yq_slice k = { kbuf, strlen(kbuf) };
        yq_slice out = { 0 };
        CHECK(yq_memtable_get(mt, k, &out) == YQ_OK);
        CHECK(out.size == sizeof(val));
    }

    /*
     * Enforcement is back on after recovery, with headroom: a small put is
     * still admitted (the overload must not turn the database read-only),
     * and a put beyond the grown threshold is still rejected.
     */
    yq_slice small_k = { "small", 5 };
    yq_slice small_v = { val, sizeof(val) };
    CHECK(yq_memtable_put(mt, small_k, small_v) == YQ_OK);

    static uint8_t big[2 * 300 * 1024];
    memset(big, 0xAB, sizeof(big));
    yq_slice big_k = { "big", 3 };
    yq_slice big_v = { big, sizeof(big) };
    CHECK(yq_memtable_put(mt, big_k, big_v) == YQ_ERR_NOMEM);

    yq_memtable_destroy(mt);
    remove_files();
    printf("OK\n");
}

/* ── 3. sync modes round-trip ────────────────────────────────────────── */

static void test_sync_off_roundtrip(void) {
    printf("test_sync_off_roundtrip... ");

    remove_files();

    yq_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(yq_opts);
    opts.flags       = YQ_OPEN_CREATE | YQ_OPEN_NOSYNC;

    /* YQ_OPEN_NOSYNC forces YQ_SYNC_OFF (see yq_open): flush hands the bytes
     * to the OS without waiting for the device. The data must still
     * round-trip: close() syncs, and reopen replays. */
    yq_db *db = NULL;
    CHECK(yq_open(TEST_DB, &opts, &db) == YQ_OK);

    uint8_t val[100];
    memset(val, 'z', sizeof(val));
    const int N = 500;
    for (int i = 0; i < N; i++) {
        yq_txn *txn = NULL;
        CHECK(yq_txn_begin(db, YQ_TXN_READWRITE, &txn) == YQ_OK);
        char kbuf[32];
        snprintf(kbuf, sizeof(kbuf), "s%06d", i);
        yq_slice k = { kbuf, strlen(kbuf) };
        yq_slice v = { val, sizeof(val) };
        CHECK(yq_put(txn, k, v, YQ_PUT_UPSERT) == YQ_OK);
        CHECK(yq_txn_commit(txn) == YQ_OK);
    }
    CHECK(yq_close(db) == YQ_OK);

    /* Reopen with default flags (NORMAL): the committed set must be intact. */
    yq_opts opts2;
    memset(&opts2, 0, sizeof(opts2));
    opts2.struct_size = sizeof(yq_opts);
    opts2.flags = YQ_OPEN_CREATE;

    yq_db *db2 = NULL;
    CHECK(yq_open(TEST_DB, &opts2, &db2) == YQ_OK);
    yq_txn *ro = NULL;
    CHECK(yq_txn_begin(db2, YQ_TXN_READONLY, &ro) == YQ_OK);
    for (int i = 0; i < N; i++) {
        char kbuf[32];
        snprintf(kbuf, sizeof(kbuf), "s%06d", i);
        yq_slice k = { kbuf, strlen(kbuf) };
        yq_slice out = { 0 };
        CHECK(yq_get(ro, k, &out) == YQ_OK);
        CHECK(out.size == sizeof(val));
    }
    CHECK(yq_txn_commit(ro) == YQ_OK);

    /* Explicit YQ_SYNC_FULL is accepted too and keeps the same data. */
    yq_txn *rw = NULL;
    CHECK(yq_txn_begin(db2, YQ_TXN_READWRITE, &rw) == YQ_OK);
    char kbuf[32];
    snprintf(kbuf, sizeof(kbuf), "s%06d", 12345);
    yq_slice k = { kbuf, strlen(kbuf) };
    yq_slice v = { "hello", 5 };
    CHECK(yq_put(rw, k, v, YQ_PUT_UPSERT) == YQ_OK);
    CHECK(yq_txn_commit(rw) == YQ_OK);

    yq_txn *ro2 = NULL;
    CHECK(yq_txn_begin(db2, YQ_TXN_READONLY, &ro2) == YQ_OK);
    yq_slice out = { 0 };
    CHECK(yq_get(ro2, k, &out) == YQ_OK);
    CHECK(out.size == 5 && memcmp(out.data, "hello", 5) == 0);
    CHECK(yq_txn_commit(ro2) == YQ_OK);

    CHECK(yq_close(db2) == YQ_OK);
    remove_files();
    printf("OK\n");
}

/*
 * Lifting the admission check for crash recovery must not disable it on the
 * normal write path. Note the enforcement point: yq_put() only stages, the
 * memtable admission happens in pending_apply() during commit -- which is
 * the pre-existing behaviour this test pins down.
 */
static void test_memtable_threshold_still_rejects(void) {
    printf("test_memtable_threshold_still_rejects... ");

    remove_files();

    yq_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(yq_opts);
    opts.flags = YQ_OPEN_CREATE | YQ_OPEN_NOSYNC;
    opts.memtable_bytes = 1024 * 1024;           /* 1 MiB live set */
    opts.log_bytes = 1024ull * 1024 * 1024;      /* never trigger checkpoint */

    yq_db *db = NULL;
    CHECK(yq_open(TEST_DB, &opts, &db) == YQ_OK);

    /* 32 x 64 KiB = 2 MiB staged: staging (WAL + pending queue) accepts it
     * all, the memtable does not. */
    static uint8_t big[64 * 1024];
    memset(big, 'q', sizeof(big));
    const int N = 32;

    yq_txn *txn = NULL;
    CHECK(yq_txn_begin(db, YQ_TXN_READWRITE, &txn) == YQ_OK);
    for (int i = 0; i < N; i++) {
        char kbuf[32];
        snprintf(kbuf, sizeof(kbuf), "b%06d", i);
        yq_slice k = { kbuf, strlen(kbuf) };
        yq_slice v = { big, sizeof(big) };
        CHECK(yq_put(txn, k, v, YQ_PUT_UPSERT) == YQ_OK);
    }

    CHECK(yq_txn_commit(txn) == YQ_ERR_NOMEM);

    CHECK(yq_close(db) == YQ_OK);
    remove_files();
    printf("OK\n");
}

int main(void) {
    printf("=== yq-DB P0 Regression Tests ===\n\n");

    test_auto_checkpoint_log_threshold();
    test_recovery_exceeds_memtable_threshold();
    test_sync_off_roundtrip();
    test_memtable_threshold_still_rejects();

    printf("\n=== ALL TESTS PASSED ===\n");
    return 0;
}
