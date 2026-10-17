/*
 * yq_test_tornwal.c -- crash consistency of the append log.
 *
 * The log is the only durable copy of the dataset (the B+Tree is not yet
 * spilled to, see yq_checkpoint()), so a torn tail is the single scariest
 * failure mode in the engine: if recovery reacts badly to a half-written
 * record it can lose commits that were already fsynced and reported to the
 * caller.
 *
 * A crash can only ever tear the *last* record being written, which means the
 * only acceptable outcome is a prefix: after recovery the database must hold
 * exactly the transactions whose COMMIT record survived, never fewer and never
 * a mixture. This test walks a truncation point through the log one byte at a
 * time and checks that property directly, because nothing else in the suite
 * looks at recovery with a damaged log at all.
 *
 * How the fixture works:
 *
 *   1. Write N single-key transactions with YQ_SYNC_FULL, so each COMMIT is on
 *      disk (and durable) before the next one starts.
 *   2. Snapshot <db> and <db>.log as bytes. The snapshot is what a crash would
 *      leave behind; nothing is closed, so no checkpoint/compaction collapses
 *      the log into a single transaction.
 *   3. For every cut length 0..len: restore the snapshot, write the first `cut`
 *      bytes of the log, reopen, and count the visible keys.
 *
 * Invariants asserted:
 *
 *   - reopening a database whose log is missing any number of tail bytes always
 *     succeeds (it is a crash, not corruption of the middle of the file);
 *   - the key count is monotone non-decreasing in `cut` -- restoring more of the
 *     log can never make recovery forget a transaction it already replayed;
 *   - every prefix 0..N is actually reachable, i.e. the engine really does
 *     recover transaction-by-transaction rather than all-or-nothing;
 *   - the full log recovers all N keys.
 *
 * A second case covers the compacted log, where the whole live set is written
 * as one transaction: there the prefix property degenerates to atomicity --
 * recovery must yield either none of the keys or all of them, never a partial
 * mix.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yq.h"

#define N_TXN 8
#define MAX_LOG (1u << 20)

static const char *TEST_DB = "yqtest_tornwal.yqdb";

static int g_fail = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("        FAIL: %s (line %d)\n", #cond, __LINE__);           \
            g_fail++;                                                          \
        }                                                                      \
    } while (0)

static void aux_name(char *out, size_t cap, const char *suffix) {
    snprintf(out, cap, "%s%s", TEST_DB, suffix);
}

static void remove_all(void) {
    char buf[128];
    remove(TEST_DB);
    aux_name(buf, sizeof buf, ".log");  remove(buf);
    aux_name(buf, sizeof buf, ".shm");  remove(buf);
    aux_name(buf, sizeof buf, ".lock"); remove(buf);
}

static size_t read_file(const char *path, unsigned char *buf, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

static int write_bytes(const char *path, const unsigned char *buf, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t wrote = n ? fwrite(buf, 1, n, f) : 0;
    int ok = (fclose(f) == 0) && wrote == n;
    return ok ? 0 : -1;
}

/* Open, count every visible key through a cursor, close. Returns -1 on error. */
static int count_keys(void) {
    char log_path[128], shm_path[128];
    aux_name(log_path, sizeof log_path, ".log");
    aux_name(shm_path, sizeof shm_path, ".shm");
    (void)log_path;
    /* The reader table is not persistent; a stale one from the previous
     * iteration must not leak slots into this one. */
    remove(shm_path);

    yq_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.struct_size = sizeof opts;
    yq_db *db = NULL;
    int rc = yq_open(TEST_DB, &opts, &db);
    if (rc != YQ_OK) return -1;

    yq_txn *txn = NULL;
    if (yq_txn_begin(db, YQ_TXN_READONLY, &txn) != YQ_OK) {
        yq_close(db);
        return -1;
    }
    int n = 0;
    yq_cur *cur = NULL;
    if (yq_cur_open(txn, &cur) == YQ_OK) {
        for (int r = yq_cur_first(cur); r == YQ_OK; r = yq_cur_next(cur)) n++;
        yq_cur_close(cur);
    }
    yq_txn_commit(txn);
    yq_close(db);
    return n;
}

/* Build N single-key transactions and snapshot db + log bytes. */
static int build_snapshot(unsigned char **db_img, size_t *db_len,
                          unsigned char **log_img, size_t *log_len) {
    remove_all();

    yq_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.struct_size = sizeof opts;
    opts.flags = YQ_OPEN_CREATE;
    opts.sync_mode = YQ_SYNC_FULL; /* every COMMIT durable before the next */

    yq_db *db = NULL;
    int rc = yq_open(TEST_DB, &opts, &db);
    if (rc != YQ_OK) {
        printf("        setup: yq_open failed: %d (%s)\n", rc, yq_strerror(rc));
        return -1;
    }
    for (int i = 0; i < N_TXN; i++) {
        yq_txn *txn = NULL;
        if ((rc = yq_txn_begin(db, YQ_TXN_READWRITE, &txn)) != YQ_OK) break;
        char k[32], v[32];
        int kl = snprintf(k, sizeof k, "key%02d", i);
        int vl = snprintf(v, sizeof v, "val%02d", i);
        if ((rc = yq_put(txn, (yq_slice){ k, (size_t)kl },
                         (yq_slice){ v, (size_t)vl }, YQ_PUT_UPSERT)) != YQ_OK) {
            yq_txn_abort(txn);
            break;
        }
        if ((rc = yq_txn_commit(txn)) != YQ_OK) break;
    }
    if (rc != YQ_OK) {
        printf("        setup: write failed: %d (%s)\n", rc, yq_strerror(rc));
        yq_close(db);
        return -1;
    }

    /* Snapshot before close: close() compacts the log into one transaction,
     * and the point of this test is the ordinary multi-transaction log. */
    char log_path[128];
    aux_name(log_path, sizeof log_path, ".log");

    *db_img = malloc(1 << 16);
    *log_img = malloc(MAX_LOG);
    if (!*db_img || !*log_img) {
        yq_close(db);
        return -1;
    }
    *db_len = read_file(TEST_DB, *db_img, 1 << 16);
    *log_len = read_file(log_path, *log_img, MAX_LOG);

    yq_close(db);
    return (*db_len && *log_len) ? 0 : -1;
}

/* Restore the snapshot with the log truncated to `cut` bytes, then reopen. */
static int recover_with_cut(const unsigned char *db_img, size_t db_len,
                            const unsigned char *log_img, size_t cut) {
    char log_path[128];
    aux_name(log_path, sizeof log_path, ".log");
    if (write_bytes(TEST_DB, db_img, db_len) != 0) return -1;
    if (write_bytes(log_path, log_img, cut) != 0) return -1;
    return count_keys();
}

static void test_truncation_is_a_prefix(void) {
    printf("test_truncation_is_a_prefix... ");
    int before = g_fail;

    unsigned char *db_img = NULL, *log_img = NULL;
    size_t db_len = 0, log_len = 0;
    if (build_snapshot(&db_img, &db_len, &log_img, &log_len) != 0) {
        printf("FAIL (fixture)\n");
        g_fail++;
        free(db_img);
        free(log_img);
        return;
    }
    printf("[log=%zu bytes, %d txns] ", log_len, N_TXN);

    int prev = -1;
    unsigned char seen[N_TXN + 1];
    memset(seen, 0, sizeof seen);

    for (size_t cut = 0; cut <= log_len; cut++) {
        int n = recover_with_cut(db_img, db_len, log_img, cut);
        if (n < 0) {
            printf("\n        FAIL: reopen failed at cut=%zu\n", cut);
            g_fail++;
            break;
        }
        if (n > N_TXN) {
            printf("\n        FAIL: %d keys recovered from a %d-transaction log (cut=%zu)\n",
                   n, N_TXN, cut);
            g_fail++;
            break;
        }
        /* Monotone: more surviving bytes can never lose a replayed commit. */
        if (n < prev) {
            printf("\n        FAIL: key count went backwards (%d -> %d) at cut=%zu\n",
                   prev, n, cut);
            g_fail++;
            break;
        }
        seen[n] = 1;
        prev = n;
    }

    /*
     * Every committed prefix must be reachable, not just the two endpoints: an
     * all-or-nothing recovery would pass the monotonicity check above.
     *
     * This is a statement about the durability contract, not about the on-disk
     * layout: each transaction commits independently (YQ_SYNC_FULL makes the
     * COMMIT durable before the next transaction starts), so for every k there
     * is some crash point that leaves exactly the first k commits intact.
     * Truncating at that point must recover exactly k keys.
     *
     * If this ever fails, recovery is not prefix-consistent: some committed
     * transaction became unrecoverable while a *later* one survived. That is a
     * durability regression, not a test-fixture detail -- so the check is
     * deliberately not weakened to "monotone + endpoints".
     */
    int gaps = 0;
    for (int k = 0; k <= N_TXN; k++) {
        if (!seen[k]) {
            printf("\n        FAIL: no truncation point recovers exactly %d keys", k);
            gaps++;
        }
    }
    if (gaps) {
        printf(" (%d of %d prefixes unreachable -- recovery is not prefix-consistent)\n",
               gaps, N_TXN + 1);
        g_fail++;
    }
    CHECK(prev == N_TXN); /* the intact log recovers everything */

    free(db_img);
    free(log_img);
    printf("%s\n", g_fail == before ? "PASS" : "FAIL");
}

/*
 * After a clean close the log holds the entire live set as a single
 * transaction. A torn tail there cannot recover a subset without inventing a
 * partial transaction, so the only acceptable outcomes are all or nothing.
 */
static void test_compacted_log_is_atomic(void) {
    printf("test_compacted_log_is_atomic... ");
    int before = g_fail;

    unsigned char *db_img = NULL, *log_img = NULL;
    size_t db_len = 0, log_len = 0;
    if (build_snapshot(&db_img, &db_len, &log_img, &log_len) != 0) {
        printf("FAIL (fixture)\n");
        g_fail++;
        free(db_img);
        free(log_img);
        return;
    }

    /* Reopen once and close cleanly: that is the checkpoint that compacts. */
    char log_path[128];
    aux_name(log_path, sizeof log_path, ".log");
    if (recover_with_cut(db_img, db_len, log_img, log_len) != N_TXN) {
        printf("FAIL (pre-close reopen)\n");
        g_fail++;
        free(db_img);
        free(log_img);
        return;
    }
    size_t compacted_len = read_file(log_path, log_img, MAX_LOG);
    size_t db_len2 = read_file(TEST_DB, db_img, 1 << 16);
    printf("[compacted log=%zu bytes] ", compacted_len);

    for (size_t cut = 0; cut <= compacted_len; cut++) {
        int n = recover_with_cut(db_img, db_len2, log_img, cut);
        if (n < 0) {
            printf("\n        FAIL: reopen failed at compacted cut=%zu\n", cut);
            g_fail++;
            break;
        }
        if (n != 0 && n != N_TXN) {
            printf("\n        FAIL: partial transaction recovered: %d of %d keys at cut=%zu\n",
                   n, N_TXN, cut);
            g_fail++;
            break;
        }
    }

    free(db_img);
    free(log_img);
    printf("%s\n", g_fail == before ? "PASS" : "FAIL");
}

int main(void) {
    test_truncation_is_a_prefix();
    test_compacted_log_is_atomic();

    remove_all();

    if (g_fail) {
        printf("\ntornwal_test: %d check(s) FAILED\n", g_fail);
        return 1;
    }
    printf("\ntornwal_test: crash recovery is prefix-consistent\n");
    return 0;
}
