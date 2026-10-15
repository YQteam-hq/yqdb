/*
 * yq_test_pending.c - the in-transaction pending store: arena allocation and
 * the O(1) pending index.
 *
 * Every yq_put()/yq_del() stages its write in the transaction before commit.
 * That store just changed shape twice:
 *
 *   - the key+value block comes from a database-level arena (yq_mempool) when
 *     it fits, and from malloc when it does not or when another transaction
 *     owns the arena;
 *   - pending_find() resolves through an open-addressing index instead of a
 *     backwards linear scan, with the scan kept as a fallback.
 *
 * Both are invisible if the semantics hold, so these tests pin the semantics
 * and exercise every ownership path:
 *
 *   1. reads inside a transaction see the newest staged value (last-write-wins)
 *      and a staged delete shadows earlier writes;
 *   2. YQ_PUT_NOOVERWRITE still reports YQ_ERR_EXISTS for a staged key;
 *   3. small blocks (arena) and >256-byte values (malloc) interleaved in one
 *      transaction, verified before and after commit;
 *   4. a read-only transaction running alongside the writer that owns the
 *      arena: its snapshot must not see the writer's staged writes;
 *   5. a large transaction that grows the index and the arena across several
 *      chunks, verified after commit and again after reopening the database;
 *   6. abort leaves nothing behind.
 *
 * A timing block at the end reports the NOOVERWRITE-heavy loop, which is the
 * case the index was added for; it is printed, not asserted, so the test stays
 * deterministic across machines.
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
#define SMALL_VAL 32      /* fits the arena (limit is 256) */
#define BIG_VAL   1024    /* does not: takes the malloc fallback */

static void make_key(uint64_t id, unsigned char k[KEYLEN]) {
    memset(k, 'K', KEYLEN);
    for (int i = 0; i < 8; i++) {
        k[i] = (unsigned char)(id >> (56 - 8 * i));
    }
}

static void make_val(uint64_t n, unsigned char *v, size_t len) {
    memset(v, 0, len);
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

static yq_db *open_db(const char *path) {
    yq_opts o;
    memset(&o, 0, sizeof(o));
    o.struct_size = sizeof(o);
    o.flags = YQ_OPEN_CREATE;
    yq_db *db = NULL;
    CHECK_EQ(yq_open(path, &o, &db), YQ_OK);
    return db;
}

int main(void) {
    const char *path = "yq_test_pending.dat";
    remove_all(path);

    unsigned char k[KEYLEN], v[BIG_VAL], got[BIG_VAL];
    yq_txn *txn = NULL;
    yq_slice ks, vs;

    /* ---- staged reads, last-write-wins, delete shadowing ---- */
    yq_db *db = open_db(path);
    CHECK_EQ(yq_txn_begin(db, YQ_TXN_READWRITE, &txn), YQ_OK);

    make_key(1, k);
    make_val(0x11, v, SMALL_VAL);
    ks.data = k; ks.size = KEYLEN;
    vs.data = v; vs.size = SMALL_VAL;
    CHECK_EQ(yq_put(txn, ks, vs, YQ_PUT_UPSERT), YQ_OK);

    make_key(1, k);                                  /* same key again */
    make_val(0x22, v, SMALL_VAL);
    CHECK_EQ(yq_put(txn, ks, vs, YQ_PUT_UPSERT), YQ_OK);

    {   /* the newest staged value is the visible one */
        yq_slice out; out.data = NULL; out.size = 0;
        CHECK_EQ(yq_get(txn, ks, &out), YQ_OK);
        CHECK_EQ(out.size, SMALL_VAL);
        CHECK_EQ(memcmp(out.data, v, SMALL_VAL), 0);
    }
    {   /* NOOVERWRITE sees the staged key */
        make_val(0x33, v, SMALL_VAL);
        CHECK_EQ(yq_put(txn, ks, vs, YQ_PUT_NOOVERWRITE), YQ_ERR_EXISTS);
    }

    /* a staged delete shadows the earlier writes */
    make_key(2, k);
    make_val(0x44, v, SMALL_VAL);
    CHECK_EQ(yq_put(txn, ks, vs, YQ_PUT_UPSERT), YQ_OK);
    CHECK_EQ(yq_del(txn, ks), YQ_OK);
    {
        yq_slice out; out.data = NULL; out.size = 0;
        CHECK_EQ(yq_get(txn, ks, &out), YQ_ERR_NOTFOUND);
        /* and the key is stageable again afterwards */
        make_val(0x55, v, SMALL_VAL);
        CHECK_EQ(yq_put(txn, ks, vs, YQ_PUT_UPSERT), YQ_OK);
        CHECK_EQ(yq_get(txn, ks, &out), YQ_OK);
        CHECK_EQ(memcmp(out.data, v, SMALL_VAL), 0);
    }
    printf("ok  staged reads, last-write-wins, delete shadowing\n");

    /* ---- arena and malloc blocks interleaved in one transaction ---- */
    for (int i = 0; i < 200; i++) {
        make_key((uint64_t)(1000 + i), k);
        size_t len = (i % 3 == 0) ? BIG_VAL : SMALL_VAL;
        make_val((uint64_t)i, v, len);
        vs.size = len;
        CHECK_EQ(yq_put(txn, ks, vs, YQ_PUT_UPSERT), YQ_OK);
    }
    for (int i = 0; i < 200; i++) {
        make_key((uint64_t)(1000 + i), k);
        size_t len = (i % 3 == 0) ? BIG_VAL : SMALL_VAL;
        yq_slice out; out.data = NULL; out.size = 0;
        CHECK_EQ(yq_get(txn, ks, &out), YQ_OK);
        CHECK_EQ(out.size, len);
        make_val((uint64_t)i, v, len);
        CHECK_EQ(memcmp(out.data, v, len), 0);
    }
    CHECK_EQ(yq_txn_commit(txn), YQ_OK);

    /* after the commit the data has to come from the memtable, not the
     * transaction's own staged copy -- which no longer exists */
    {
        yq_txn *r = NULL;
        CHECK_EQ(yq_txn_begin(db, YQ_TXN_READONLY, &r), YQ_OK);
        for (int i = 0; i < 200; i++) {
            make_key((uint64_t)(1000 + i), k);
            size_t len = (i % 3 == 0) ? BIG_VAL : SMALL_VAL;
            yq_slice out; out.data = NULL; out.size = 0;
            CHECK_EQ(yq_get(r, ks, &out), YQ_OK);
            CHECK_EQ(out.size, len);
            make_val((uint64_t)i, v, len);
            CHECK_EQ(memcmp(out.data, v, len), 0);
        }
        CHECK_EQ(yq_txn_abort(r), YQ_OK);
    }
    printf("ok  arena (small) and malloc (big) blocks in one transaction\n");

    /* ---- abort leaves nothing behind ---- */
    CHECK_EQ(yq_txn_begin(db, YQ_TXN_READWRITE, &txn), YQ_OK);
    make_key(999999, k);
    make_val(0xDEAD, v, SMALL_VAL);
    vs.size = SMALL_VAL;
    CHECK_EQ(yq_put(txn, ks, vs, YQ_PUT_UPSERT), YQ_OK);
    CHECK_EQ(yq_txn_abort(txn), YQ_OK);
    {
        yq_txn *r = NULL;
        CHECK_EQ(yq_txn_begin(db, YQ_TXN_READONLY, &r), YQ_OK);
        yq_slice out; out.data = NULL; out.size = 0;
        CHECK_EQ(yq_get(r, ks, &out), YQ_ERR_NOTFOUND);
        CHECK_EQ(yq_txn_abort(r), YQ_OK);
    }
    printf("ok  abort discards staged writes\n");

    /* ---- a read-only txn while a write txn holds the arena ----
     *
     * The engine elects a single writer (yq_mvcc_elect_writer), so two
     * read-write transactions cannot coexist and the arena's ownership flag is
     * never contended in practice. The concurrency that does exist is a reader
     * running alongside the writer, and it must keep seeing the snapshot it
     * began with, not the writer's staged writes. */
    {
        yq_txn *w = NULL, *r = NULL;
        CHECK_EQ(yq_txn_begin(db, YQ_TXN_READWRITE, &w), YQ_OK);

        make_key(500001, k);
        make_val(1, v, SMALL_VAL);
        vs.size = SMALL_VAL;
        CHECK_EQ(yq_put(w, ks, vs, YQ_PUT_UPSERT), YQ_OK);   /* arena block */

        /* the reader begins now: it must not see the uncommitted write */
        CHECK_EQ(yq_txn_begin(db, YQ_TXN_READONLY, &r), YQ_OK);
        {
            yq_slice out; out.data = NULL; out.size = 0;
            CHECK_EQ(yq_get(r, ks, &out), YQ_ERR_NOTFOUND);
        }

        /* the writer stages a big (malloc-path) value too, then commits */
        make_key(500002, k);
        make_val(2, v, BIG_VAL);
        vs.size = BIG_VAL;
        CHECK_EQ(yq_put(w, ks, vs, YQ_PUT_UPSERT), YQ_OK);
        CHECK_EQ(yq_txn_commit(w), YQ_OK);

        /*
         * The reader now sees the committed value -- and that is the engine's
         * documented behaviour, not an accident of this test: yq_get() reads
         * the CURRENT memtable and never filters it by snapshot_txn, so a
         * reader sees writes committed after its snapshot. Generational
         * memtables (FORMAT.md 8.4) are what would fix that, and they are not
         * implemented. Asserting isolation here would be asserting a property
         * the engine does not have.
         */
        {
            /* ks points at the buffer k, so it has to be re-made here: after
             * make_key(500002, k) it would name the other key. */
            make_key(500001, k);
            yq_slice out; out.data = NULL; out.size = 0;
            CHECK_EQ(yq_get(r, ks, &out), YQ_OK);
            make_val(1, v, SMALL_VAL);
            CHECK_EQ(out.size, SMALL_VAL);
            CHECK_EQ(memcmp(out.data, v, SMALL_VAL), 0);
            CHECK_EQ(yq_txn_abort(r), YQ_OK);
        }
    }
    {
        yq_txn *r = NULL;
        CHECK_EQ(yq_txn_begin(db, YQ_TXN_READONLY, &r), YQ_OK);
        struct { uint64_t id; size_t len; uint64_t val; } want[] = {
            {500001, SMALL_VAL, 1}, {500002, BIG_VAL, 2}
        };
        for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
            make_key(want[i].id, k);
            yq_slice out; out.data = NULL; out.size = 0;
            CHECK_EQ(yq_get(r, ks, &out), YQ_OK);
            CHECK_EQ(out.size, want[i].len);
            make_val(want[i].val, v, want[i].len);
            CHECK_EQ(memcmp(out.data, v, want[i].len), 0);
        }
        CHECK_EQ(yq_txn_abort(r), YQ_OK);
    }
    printf("ok  reader alongside a writer holding the arena\n");

    /* ---- a large transaction: index growth and several arena chunks ---- */
    {
        const int N = 20000;
        CHECK_EQ(yq_txn_begin(db, YQ_TXN_READWRITE, &txn), YQ_OK);
        for (int i = 0; i < N; i++) {
            make_key((uint64_t)(100000 + i), k);
            make_val((uint64_t)i, v, SMALL_VAL);
            vs.size = SMALL_VAL;
            CHECK_EQ(yq_put(txn, ks, vs, YQ_PUT_UPSERT), YQ_OK);
            /* rewrite every 8th key so the index has to collapse duplicates */
            if (i % 8 == 0) {
                make_val((uint64_t)i + 0x1000000, v, SMALL_VAL);
                CHECK_EQ(yq_put(txn, ks, vs, YQ_PUT_UPSERT), YQ_OK);
            }
        }
        for (int i = 0; i < N; i++) {
            make_key((uint64_t)(100000 + i), k);
            yq_slice out; out.data = NULL; out.size = 0;
            CHECK_EQ(yq_get(txn, ks, &out), YQ_OK);
            uint64_t want = (uint64_t)i + ((i % 8 == 0) ? 0x1000000 : 0);
            make_val(want, v, SMALL_VAL);
            CHECK_EQ(memcmp(out.data, v, SMALL_VAL), 0);
        }
        CHECK_EQ(yq_txn_commit(txn), YQ_OK);
        printf("ok  %d-key transaction (index + arena growth)\n", N);
    }

    CHECK_EQ(yq_close(db), YQ_OK);

    /* ---- everything survives a reopen ---- */
    db = open_db(path);
    {
        yq_txn *r = NULL;
        CHECK_EQ(yq_txn_begin(db, YQ_TXN_READONLY, &r), YQ_OK);

        const int N = 20000;
        for (int i = 0; i < N; i++) {
            make_key((uint64_t)(100000 + i), k);
            yq_slice out; out.data = NULL; out.size = 0;
            CHECK_EQ(yq_get(r, ks, &out), YQ_OK);
            uint64_t want = (uint64_t)i + ((i % 8 == 0) ? 0x1000000 : 0);
            make_val(want, v, SMALL_VAL);
            CHECK_EQ(memcmp(out.data, v, SMALL_VAL), 0);
        }
        for (int i = 0; i < 200; i++) {
            make_key((uint64_t)(1000 + i), k);
            size_t len = (i % 3 == 0) ? BIG_VAL : SMALL_VAL;
            yq_slice out; out.data = NULL; out.size = 0;
            CHECK_EQ(yq_get(r, ks, &out), YQ_OK);
            CHECK_EQ(out.size, len);
            make_val((uint64_t)i, v, len);
            CHECK_EQ(memcmp(out.data, v, len), 0);
        }
        /* the key staged as 1 and then rewritten as 0x22 */
        make_key(1, k);
        {
            yq_slice out; out.data = NULL; out.size = 0;
            CHECK_EQ(yq_get(r, ks, &out), YQ_OK);
            make_val(0x22, v, SMALL_VAL);
            CHECK_EQ(memcmp(out.data, v, SMALL_VAL), 0);
        }
        CHECK_EQ(yq_txn_abort(r), YQ_OK);
    }
    CHECK_EQ(yq_close(db), YQ_OK);
    remove_all(path);
    printf("ok  all staged data survives reopen\n");

    /* ---- informational: the pending lookup the index was added for ----
     *
     * Two shapes, because they measure different things:
     *
     *   hot  - each NOOVERWRITE immediately follows its own put. A backwards
     *          linear scan finds the key on its first probe, so this measures
     *          the cost of staging and must not regress.
     *   cold - all N keys are staged first, then probed oldest-first. The
     *          backwards scan then has to walk the whole array for the first
     *          key and one step less for each of the next, i.e. O(N^2) key
     *          comparisons. This is the loop that made a 200k-write
     *          transaction take tens of seconds.
     */
    {
        const int N = 20000;
        db = open_db(path);

        CHECK_EQ(yq_txn_begin(db, YQ_TXN_READWRITE, &txn), YQ_OK);
        double t0 = now_sec();
        for (int i = 0; i < N; i++) {
            make_key((uint64_t)i, k);
            make_val((uint64_t)i, v, SMALL_VAL);
            vs.size = SMALL_VAL;
            CHECK_EQ(yq_put(txn, ks, vs, YQ_PUT_UPSERT), YQ_OK);
            CHECK_EQ(yq_put(txn, ks, vs, YQ_PUT_NOOVERWRITE), YQ_ERR_EXISTS);
        }
        double dt_hot = now_sec() - t0;
        CHECK_EQ(yq_txn_commit(txn), YQ_OK);
        CHECK_EQ(yq_close(db), YQ_OK);
        remove_all(path);

        db = open_db(path);
        CHECK_EQ(yq_txn_begin(db, YQ_TXN_READWRITE, &txn), YQ_OK);
        t0 = now_sec();
        for (int i = 0; i < N; i++) {
            make_key((uint64_t)i, k);
            make_val((uint64_t)i, v, SMALL_VAL);
            vs.size = SMALL_VAL;
            CHECK_EQ(yq_put(txn, ks, vs, YQ_PUT_UPSERT), YQ_OK);
        }
        for (int i = 0; i < N; i++) {
            make_key((uint64_t)i, k);           /* oldest first: worst case */
            CHECK_EQ(yq_put(txn, ks, vs, YQ_PUT_NOOVERWRITE), YQ_ERR_EXISTS);
        }
        double dt_cold = now_sec() - t0;
        CHECK_EQ(yq_txn_commit(txn), YQ_OK);
        printf("info  %d staged + %d NOOVERWRITE:  hot %.3f s   cold (oldest-first) %.3f s\n",
               N, N, dt_hot, dt_cold);
        CHECK_EQ(yq_close(db), YQ_OK);
        remove_all(path);
    }

    printf("pending_test: all cases passed\n");
    return 0;
}
