/*
 * yq_test_durability.c — a commit must never claim success when it could not
 * be made durable.
 *
 * yq_txn_commit() used to ignore the return value of yq_wal_flush() and of the
 * meta-page write, so an I/O failure while persisting a commit was invisible:
 * the call returned YQ_OK even though nothing had reached the disk.
 *
 * This test forces that failure deterministically. On POSIX, RLIMIT_FSIZE caps
 * how far a file may grow; a write that would push a file past the cap fails
 * with EFBIG (and delivers SIGXFSZ, which we ignore). Once the cap sits below
 * what the next commit needs, yq_txn_commit() must report the failure.
 *
 * The Windows branch cannot raise that failure portably, so it only checks the
 * happy path (commit, close, reopen, read back) to keep the test meaningful on
 * every platform.
 */
#include "yq.h"
#include "yq_test_check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <signal.h>
#include <sys/resource.h>
#endif

static void remove_all(const char *path) {
    char buf[512];
    remove(path);
    snprintf(buf, sizeof(buf), "%s.log", path);
    remove(buf);
    snprintf(buf, sizeof(buf), "%s.shm", path);
    remove(buf);
    snprintf(buf, sizeof(buf), "%s.lock", path);
    remove(buf);
}

int main(void) {
    const char *path = "yq_test_durability.dat";
    remove_all(path);

    yq_opts o;
    memset(&o, 0, sizeof(o));
    o.struct_size = sizeof(o);
    o.flags = YQ_OPEN_CREATE;
    o.sync_mode = YQ_SYNC_NORMAL;

    yq_db *db = NULL;
    CHECK_EQ(yq_open(path, &o, &db), YQ_OK);

    /* A normal commit succeeds. */
    yq_txn *txn = NULL;
    CHECK_EQ(yq_txn_begin(db, YQ_TXN_READWRITE, &txn), YQ_OK);
    yq_slice k = { "alpha", 5 };
    yq_slice v = { "one", 3 };
    CHECK_EQ(yq_put(txn, k, v, YQ_PUT_UPSERT), YQ_OK);
    CHECK_EQ(yq_txn_commit(txn), YQ_OK);

#if defined(_WIN32)

    /* Happy-path durability: the value survives close + reopen. */
    CHECK_EQ(yq_close(db), YQ_OK);
    db = NULL;
    CHECK_EQ(yq_open(path, &o, &db), YQ_OK);
    {
        yq_txn *r = NULL;
        yq_slice out = { 0 };
        CHECK_EQ(yq_txn_begin(db, YQ_TXN_READONLY, &r), YQ_OK);
        CHECK_EQ(yq_get(r, k, &out), YQ_OK);
        CHECK_EQ(out.size, 3);
        CHECK_EQ(yq_txn_commit(r), YQ_OK);
    }
    CHECK_EQ(yq_close(db), YQ_OK);
    remove_all(path);
    printf("durability: OK (happy path)\n");
    return 0;

#else

    /*
     * Cap the file size below what the next commit needs, so the WAL flush
     * cannot succeed. Ignore SIGXFSZ so the kernel returns EFBIG to pwrite()
     * instead of terminating the process.
     */
    signal(SIGXFSZ, SIG_IGN);

    struct rlimit rl;
    CHECK_EQ(getrlimit(RLIMIT_FSIZE, &rl), 0);
    struct rlimit tight = rl;
    tight.rlim_cur = 64 * 1024; /* 64 KiB */
    CHECK_EQ(setrlimit(RLIMIT_FSIZE, &tight), 0);

    /* A commit appending a 1 MiB record cannot fit under the 64 KiB cap. */
    const size_t big = 1024 * 1024;
    char *val = (char *)malloc(big);
    CHECK(val != NULL);
    memset(val, 'x', big);

    yq_txn *txn2 = NULL;
    CHECK_EQ(yq_txn_begin(db, YQ_TXN_READWRITE, &txn2), YQ_OK);
    yq_slice k2 = { "beta", 4 };
    yq_slice v2 = { val, big };
    CHECK_EQ(yq_put(txn2, k2, v2, YQ_PUT_UPSERT), YQ_OK);

    int crc = yq_txn_commit(txn2);
    /* Must NOT report success for a commit that never reached the disk. */
    CHECK(crc != YQ_OK);

    free(val);

    /* Restore the cap so the remaining cleanup can create/remove files. */
    CHECK_EQ(setrlimit(RLIMIT_FSIZE, &rl), 0);

    yq_close(db);
    remove_all(path);

    printf("durability: OK (forced write failure reported)\n");
    return 0;

#endif
}
