/*
 * yq_test_meta_reads.c — the commit path must read the meta-page pair once.
 *
 * yq_txn_commit() used to call yq_mvcc_meta_read() (2 page reads) and then
 * yq_mvcc_meta_pwrite_full(), whose internal select_meta_index() read the same
 * two pages again -- four page reads per commit for 100 bytes of metadata.
 * yq_mvcc_meta_read_select() + yq_mvcc_meta_pwrite_at() collapse that to two.
 *
 * On POSIX the test counts pread64() calls with the linker's --wrap, so the
 * bound it asserts fails on the unoptimised code. It also checks that a
 * database written through this path survives close + reopen.
 */
#define _LARGEFILE64_SOURCE
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "yq.h"
#include "yq_test_check.h"

#if !defined(_WIN32)
#include <sys/types.h>
#include <unistd.h>

extern ssize_t __real_pread64(int fd, void *buf, size_t count, off64_t offset);
static long g_pread_calls = 0;
static int g_counting = 0;

ssize_t __wrap_pread64(int fd, void *buf, size_t count, off64_t offset) {
    if (g_counting) g_pread_calls++;
    return __real_pread64(fd, buf, count, offset);
}
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

#define TXNS 100

static void make_key(uint64_t id, char k[16]) {
    snprintf(k, 16, "key-%011llu", (unsigned long long)id);
}

static int put_one(yq_db *db, uint64_t id, const char *val, size_t vlen) {
    yq_txn *txn = NULL;
    int rc = yq_txn_begin(db, YQ_TXN_READWRITE, &txn);
    if (rc != YQ_OK) return rc;
    char k[16];
    make_key(id, k);
    yq_slice ks = { k, strlen(k) };
    yq_slice vs = { val, vlen };
    rc = yq_put(txn, ks, vs, YQ_PUT_UPSERT);
    if (rc != YQ_OK) { yq_txn_abort(txn); return rc; }
    return yq_txn_commit(txn);
}

int main(void) {
    const char *path = "yq_test_meta_reads.dat";
    remove_all(path);

    yq_opts o;
    memset(&o, 0, sizeof(o));
    o.struct_size = sizeof(o);
    o.flags = YQ_OPEN_CREATE;
    o.sync_mode = YQ_SYNC_NORMAL;

    yq_db *db = NULL;
    CHECK_EQ(yq_open(path, &o, &db), YQ_OK);

    /* Warm-up commits (also the correctness payload). */
    for (uint64_t i = 0; i < TXNS; i++) {
        CHECK_EQ(put_one(db, i, "value", 5), YQ_OK);
    }

    /* Measured commits: each is txn_begin (2 meta reads) + commit (2). */
#if !defined(_WIN32)
    g_pread_calls = 0;
    g_counting = 1;
#endif
    for (uint64_t i = 0; i < TXNS; i++) {
        CHECK_EQ(put_one(db, TXNS + i, "value", 5), YQ_OK);
    }
#if !defined(_WIN32)
    g_counting = 0;

    printf("meta_reads: %ld pread64 for %d commit transactions (%.1f/txn)\n",
           g_pread_calls, TXNS, (double)g_pread_calls / TXNS);

    /*
     * 2 reads in yq_txn_begin + 2 in yq_txn_commit = 4 per transaction. The
     * pre-change code did 6 (2 + 4), so this bound discriminates the two.
     */
    CHECK(g_pread_calls <= (long)5 * TXNS);
#endif

    /* Durability + correctness across a reopen. */
    CHECK_EQ(yq_close(db), YQ_OK);
    db = NULL;
    CHECK_EQ(yq_open(path, &o, &db), YQ_OK);
    {
        yq_txn *r = NULL;
        CHECK_EQ(yq_txn_begin(db, YQ_TXN_READONLY, &r), YQ_OK);
        char k[16];
        make_key(TXNS + TXNS - 1, k);
        yq_slice ks = { k, strlen(k) };
        yq_slice out = { 0 };
        CHECK_EQ(yq_get(r, ks, &out), YQ_OK);
        CHECK_EQ(out.size, 5);
        CHECK(memcmp(out.data, "value", 5) == 0);
        CHECK_EQ(yq_txn_commit(r), YQ_OK);
    }
    CHECK_EQ(yq_close(db), YQ_OK);

    remove_all(path);
    printf("meta_reads: OK\n");
    return 0;
}
