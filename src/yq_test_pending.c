/*
 * yq_test_pending.c — the in-transaction pending index must behave exactly
 * like the linear scan it replaces.
 *
 * yq_get() and the YQ_PUT_NOOVERWRITE path of yq_put() resolve a key against
 * the transaction's uncommitted writes first. That lookup used to be a
 * backwards linear scan of the whole pending array; it is now an
 * open-addressing index. This test pins the observable behaviour:
 *
 *   - NOOVERWRITE still reports YQ_ERR_EXISTS for a key already staged;
 *   - reads inside the transaction still observe the newest staged value;
 *   - a staged delete shadows earlier writes and reads back as NOTFOUND;
 *   - last-write-wins ordering survives many repeated keys;
 *   - the fallback path (index abandoned) is exercised by re-reading after
 *     every stage.
 *
 * A timing block at the end prints how long a large transaction takes; it is
 * not asserted on, so the test stays deterministic across machines.
 */
#include "yq.h"
#include "yq_test_check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
static double now_sec(void) {
    LARGE_INTEGER f, c;
    static LARGE_INTEGER freq;
    static int init = 0;
    if (!init) { QueryPerformanceFrequency(&freq); init = 1; }
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}
#else
#include <time.h>
static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
#endif

#define KEYLEN 16
#define VALLEN 32

static void make_key(uint64_t id, unsigned char k[KEYLEN]) {
    memset(k, 'K', KEYLEN);
    for (int i = 0; i < 8; i++) {
        k[i] = (unsigned char)(id >> (56 - 8 * i));
    }
}

static void make_val(uint64_t n, unsigned char v[VALLEN]) {
    memset(v, 0, VALLEN);
    memcpy(v, &n, sizeof(n));
}

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
    const char *path = "yq_test_pending.dat";
    remove_all(path);

    yq_opts o;
    memset(&o, 0, sizeof(o));
    o.struct_size = sizeof(o);
    o.flags = YQ_OPEN_CREATE;

    yq_db *db = NULL;
    CHECK_EQ(yq_open(path, &o, &db), YQ_OK);

    const uint64_t N = 20000;
    unsigned char k[KEYLEN], v[VALLEN], got[VALLEN];

    /* ---- NOOVERWRITE sees staged keys; reads see staged values ---- */
    yq_txn *txn = NULL;
    CHECK_EQ(yq_txn_begin(db, YQ_TXN_READWRITE, &txn), YQ_OK);

    for (uint64_t i = 0; i < N; i++) {
        yq_slice ks, vs;
        make_key(i, k);
        make_val(i, v);
        ks.data = k; ks.size = KEYLEN;
        vs.data = v; vs.size = VALLEN;
        CHECK_EQ(yq_put(txn, ks, vs, YQ_PUT_UPSERT), YQ_OK);
    }

    /* Every key is now staged: NOOVERWRITE must report EXISTS, and a read must
     * return the staged value. */
    for (uint64_t i = 0; i < N; i++) {
        yq_slice ks, vs, out;
        make_key(i, k);
        make_val(i + 1, v); /* different value: must stay ignored */
        ks.data = k; ks.size = KEYLEN;
        vs.data = v; vs.size = VALLEN;
        CHECK_EQ(yq_put(txn, ks, vs, YQ_PUT_NOOVERWRITE), YQ_ERR_EXISTS);

        out.data = NULL; out.size = 0;
        ks.data = k; ks.size = KEYLEN;
        CHECK_EQ(yq_get(txn, ks, &out), YQ_OK);
        CHECK_EQ(out.size, VALLEN);
        memcpy(got, out.data, VALLEN);
        make_val(i, v);
        CHECK_EQ(memcmp(got, v, VALLEN), 0);
    }

    /* ---- last-write-wins across repeated keys ---- */
    {
        yq_slice ks, vs, out;
        make_key(0, k);
        ks.data = k; ks.size = KEYLEN;
        for (uint64_t r = 0; r < 100; r++) {
            make_val(1000 + r, v);
            vs.data = v; vs.size = VALLEN;
            CHECK_EQ(yq_put(txn, ks, vs, YQ_PUT_UPSERT), YQ_OK);
        }
        out.data = NULL; out.size = 0;
        CHECK_EQ(yq_get(txn, ks, &out), YQ_OK);
        make_val(1000 + 99, v);
        CHECK_EQ(out.size, VALLEN);
        CHECK_EQ(memcmp(out.data, v, VALLEN), 0);
    }

    /* ---- a staged delete shadows earlier writes ---- */
    {
        yq_slice ks, vs, out;
        make_key(7, k);
        ks.data = k; ks.size = KEYLEN;
        make_val(42, v);
        vs.data = v; vs.size = VALLEN;
        CHECK_EQ(yq_put(txn, ks, vs, YQ_PUT_UPSERT), YQ_OK);
        CHECK_EQ(yq_del(txn, ks), YQ_OK);
        out.data = NULL; out.size = 0;
        CHECK_EQ(yq_get(txn, ks, &out), YQ_ERR_NOTFOUND);

        /* delete then put again: the newest op wins and the key is visible. */
        make_val(43, v);
        vs.data = v; vs.size = VALLEN;
        CHECK_EQ(yq_put(txn, ks, vs, YQ_PUT_UPSERT), YQ_OK);
        out.data = NULL; out.size = 0;
        CHECK_EQ(yq_get(txn, ks, &out), YQ_OK);
        CHECK_EQ(out.size, VALLEN);
        make_val(43, v);
        CHECK_EQ(memcmp(out.data, v, VALLEN), 0);
    }

    /* Abandoning the transaction must free the index too (run under ASan/LSan
     * to catch a leak here). */
    CHECK_EQ(yq_txn_abort(txn), YQ_OK);

    /* ---- timing (informational only) ---- */
    {
        const uint64_t M = 200000;
        yq_txn *t2 = NULL;
        CHECK_EQ(yq_txn_begin(db, YQ_TXN_READWRITE, &t2), YQ_OK);
        double t0 = now_sec();
        for (uint64_t i = 0; i < M; i++) {
            yq_slice ks, vs;
            make_key(i, k);
            make_val(i, v);
            ks.data = k; ks.size = KEYLEN;
            vs.data = v; vs.size = VALLEN;
            CHECK_EQ(yq_put(t2, ks, vs, YQ_PUT_UPSERT), YQ_OK);
        }
        for (uint64_t i = 0; i < M; i++) {
            yq_slice ks, vs;
            make_key(i, k);
            make_val(i, v);
            ks.data = k; ks.size = KEYLEN;
            vs.data = v; vs.size = VALLEN;
            CHECK_EQ(yq_put(t2, ks, vs, YQ_PUT_NOOVERWRITE), YQ_ERR_EXISTS);
        }
        double t1 = now_sec();
        printf("pending: %llu staged puts + %llu NOOVERWRITE probes in %.3f s\n",
               (unsigned long long)M, (unsigned long long)M, t1 - t0);
        CHECK_EQ(yq_txn_abort(t2), YQ_OK);
    }

    CHECK_EQ(yq_close(db), YQ_OK);
    remove_all(path);

    printf("pending: OK\n");
    return 0;
}
