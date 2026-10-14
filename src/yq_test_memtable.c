/*
 * Model-based tests for the memtable index (P1: sorted array -> chunked
 * sorted array; a skip list was tried and rejected -- see the comment at the
 * top of yq_memtable.c for the measurements).
 *
 * A reference model (a trivial linear array over a small key alphabet) is
 * driven with the same random operation sequence, and every observable of
 * the real memtable is compared against it: point lookups, full forward and
 * backward iteration, seek() boundaries, byte accounting and iterator
 * invalidation across reset().
 *
 * The alphabet is deliberately small (24 keys, values up to 200 B) so that
 * overwrites, deletions of absent keys, tombstone revivals and tombstone
 * runs in the middle of the order all happen many times over.
 *
 * Checks use an explicit macro instead of assert() so they stay active
 * regardless of NDEBUG.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yq.h"
#include "yq_memtable.h"
#include "yq_wal.h"
#include "yq_recover.h"

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            exit(1);                                                      \
        }                                                                 \
    } while (0)

/* ── deterministic PRNG (no libc rand(): stable across platforms) ───────── */

static uint64_t rng_state = 0x243F6A8885A308D3ULL;

static uint64_t rng_next(void) {
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}

static uint32_t rng_below(uint32_t n) {
    return (uint32_t)(rng_next() % n);
}

/* ── reference model ──────────────────────────────────────────────────── */

#define MODEL_KEYS 24
#define MODEL_VAL_MAX 200

typedef struct {
    int  present;                 /* live (not tombstoned) */
    int  was_seen;                /* an entry (live or tombstone) exists */
    /*
     * Accumulated accounting, in the memtable's own (quirky) terms.
     * used_bytes is only ever *reduced* when a LIVE entry is overwritten or
     * deleted; reviving a tombstone adds the new cost without reclaiming
     * the tombstone's. That is the pre-existing array-version behaviour
     * (see the unmerged fix/memtable-tombstone-revive-accounting and
     * fix/memtable-tombstone-used-bytes branches), deliberately preserved:
     * P1 changes the index structure, not the accounting formula, so
     * bytes()/full() must fire at the same thresholds as before. The model
     * therefore tracks the same accumulation instead of the ideal sum.
     */
    size_t acct;
    uint8_t val[MODEL_VAL_MAX];
    size_t val_len;
} model_slot;

#define ENTRY_COST(klen, vlen) ((klen) + (vlen) + 40)

static model_slot g_model[MODEL_KEYS];

static void model_key(char *buf, int i) {
    snprintf(buf, 16, "k%03d", i);
}

static void remove_recover_files(void) {
    remove("recover_order.db");
    remove("recover_order.db.log");
}

static void model_reset(void) {
    memset(g_model, 0, sizeof(g_model));
}

static void model_put(int i, const uint8_t *val, size_t len) {
    if (len > MODEL_VAL_MAX) len = MODEL_VAL_MAX;
    if (g_model[i].was_seen && g_model[i].present) {
        /* overwrite of a live entry reclaims its old cost */
        g_model[i].acct -= ENTRY_COST(4, g_model[i].val_len);
    }
    g_model[i].was_seen = 1;
    g_model[i].present = 1;
    memcpy(g_model[i].val, val, len);
    g_model[i].val_len = len;
    g_model[i].acct += ENTRY_COST(4, len);
}

static void model_del(int i) {
    if (g_model[i].was_seen && g_model[i].present) {
        g_model[i].acct -= ENTRY_COST(4, g_model[i].val_len);
        g_model[i].acct += ENTRY_COST(4, 0);
    } else if (!g_model[i].was_seen) {
        g_model[i].acct += ENTRY_COST(4, 0);
    }
    /* deleting a live-or-unknown key leaves a tombstone entry behind */
    g_model[i].was_seen = 1;
    g_model[i].present = 0;
    g_model[i].val_len = 0;
}

/* Lowest live key index >= i, or MODEL_KEYS when none. */
static int model_lower_bound(int i) {
    for (int k = i; k < MODEL_KEYS; k++) {
        if (g_model[k].present) return k;
    }
    return MODEL_KEYS;
}

static size_t model_bytes(void) {
    size_t total = 0;
    for (int i = 0; i < MODEL_KEYS; i++) {
        total += g_model[i].acct;   /* includes unreclaimed tombstone costs */
    }
    return total;
}

static int model_entries(void) {
    int n = 0;
    for (int i = 0; i < MODEL_KEYS; i++) n += g_model[i].was_seen;
    return n;
}

/* ── comparisons against the real memtable ────────────────────────────── */

static void verify_all_gets(yq_memtable *mt) {
    for (int i = 0; i < MODEL_KEYS; i++) {
        char kbuf[16];
        model_key(kbuf, i);
        yq_slice key = { kbuf, strlen(kbuf) };
        yq_slice out = { 0 };
        int rc = yq_memtable_get(mt, key, &out);

        if (g_model[i].present) {
            CHECK(rc == YQ_OK);
            CHECK(out.size == g_model[i].val_len);
            CHECK(out.size == 0 || memcmp(out.data, g_model[i].val, out.size) == 0);
        } else {
            CHECK(rc == YQ_ERR_NOTFOUND);
            CHECK(out.data == NULL && out.size == 0);
        }
    }
}

static void verify_forward_iteration(yq_memtable *mt) {
    yq_memtable_iter *it = NULL;
    CHECK(yq_memtable_iter_open(mt, &it) == YQ_OK);

    int expect = 0;
    /* iter_open() positions at the first live entry, so start there. */
    while (expect < MODEL_KEYS && !g_model[expect].present) expect++;

    if (expect == MODEL_KEYS) {
        CHECK(!yq_memtable_iter_valid(it));
    }
    for (int i = expect; i < MODEL_KEYS; i++) {
        if (!g_model[i].present) continue;
        CHECK(yq_memtable_iter_valid(it));

        yq_slice k = { 0 }, v = { 0 };
        CHECK(yq_memtable_iter_key(it, &k) == YQ_OK);
        CHECK(yq_memtable_iter_val(it, &v) == YQ_OK);

        char kbuf[16];
        model_key(kbuf, i);
        CHECK(k.size == strlen(kbuf));
        CHECK(memcmp(k.data, kbuf, k.size) == 0);
        CHECK(v.size == g_model[i].val_len);
        CHECK(v.size == 0 || memcmp(v.data, g_model[i].val, v.size) == 0);

        int rc = yq_memtable_iter_next(it);
        CHECK(rc == YQ_OK || rc == YQ_ERR_NOTFOUND);
    }
    CHECK(!yq_memtable_iter_valid(it));
    CHECK(yq_memtable_iter_next(it) == YQ_ERR_NOTFOUND);

    yq_memtable_iter_close(it);
}

static void verify_backward_iteration(yq_memtable *mt) {
    yq_memtable_iter *it = NULL;
    CHECK(yq_memtable_iter_open(mt, &it) == YQ_OK);
    CHECK(yq_memtable_iter_last(it) == YQ_OK ||
           yq_memtable_iter_last(it) == YQ_ERR_NOTFOUND);

    for (int i = MODEL_KEYS - 1; i >= 0; i--) {
        if (!g_model[i].present) continue;
        CHECK(yq_memtable_iter_valid(it));

        yq_slice k = { 0 };
        CHECK(yq_memtable_iter_key(it, &k) == YQ_OK);
        char kbuf[16];
        model_key(kbuf, i);
        CHECK(k.size == strlen(kbuf));
        CHECK(memcmp(k.data, kbuf, k.size) == 0);

        int rc = yq_memtable_iter_prev(it);
        CHECK(rc == YQ_OK || rc == YQ_ERR_NOTFOUND);
    }
    CHECK(!yq_memtable_iter_valid(it));
    CHECK(yq_memtable_iter_prev(it) == YQ_ERR_NOTFOUND);

    yq_memtable_iter_close(it);
}

static void verify_seeks(yq_memtable *mt) {
    /* Exact keys and the gaps between them (probe "k%03d" for every value,
     * plus a below-all and above-all probe). */
    for (int probe = -1; probe <= MODEL_KEYS; probe++) {
        char kbuf[16];
        if (probe < 0)        snprintf(kbuf, sizeof(kbuf), "a");
        else if (probe == MODEL_KEYS) snprintf(kbuf, sizeof(kbuf), "z");
        else                  model_key(kbuf, probe);

        yq_slice key = { kbuf, strlen(kbuf) };
        yq_memtable_iter *it = NULL;
        CHECK(yq_memtable_iter_open(mt, &it) == YQ_OK);
        int rc = yq_memtable_iter_seek(it, key);

        int want;
        if (probe == MODEL_KEYS) {
            want = MODEL_KEYS;                     /* "z" is above every key */
        } else {
            want = model_lower_bound(probe < 0 ? 0 : probe);
        }

        if (want == MODEL_KEYS) {
            CHECK(rc == YQ_ERR_NOTFOUND);
            CHECK(!yq_memtable_iter_valid(it));
        } else {
            CHECK(rc == YQ_OK);
            yq_slice k = { 0 };
            CHECK(yq_memtable_iter_valid(it));
            CHECK(yq_memtable_iter_key(it, &k) == YQ_OK);
            char wantk[16];
            model_key(wantk, want);
            CHECK(k.size == strlen(wantk) && memcmp(k.data, wantk, k.size) == 0);
        }
        yq_memtable_iter_close(it);
    }
}

/* ── the randomized drive ─────────────────────────────────────────────── */

static void test_model_random(void) {
    printf("test_model_random... ");

    yq_memtable *mt = yq_memtable_create(16 * 1024 * 1024);
    CHECK(mt != NULL);
    model_reset();

    const int OPS = 20000;
    for (int op = 0; op < OPS; op++) {
        int i = (int)rng_below(MODEL_KEYS);
        char kbuf[16];
        model_key(kbuf, i);
        yq_slice key = { kbuf, strlen(kbuf) };
        uint32_t roll = rng_below(100);

        if (roll < 60) {
            uint8_t val[MODEL_VAL_MAX];
            size_t len = 1 + rng_below(MODEL_VAL_MAX);
            for (size_t b = 0; b < len; b++) val[b] = (uint8_t)rng_next();
            yq_slice v = { val, len };
            CHECK(yq_memtable_put(mt, key, v) == YQ_OK);
            model_put(i, val, len);
        } else if (roll < 80) {
            CHECK(yq_memtable_del(mt, key) == YQ_OK);
            model_del(i);
        } else {
            /* delete an absent key: installs a tombstone, stays idempotent */
            CHECK(yq_memtable_del(mt, key) == YQ_OK);
            model_del(i);
        }

        verify_all_gets(mt);

        if (op % 97 == 0) {
            verify_forward_iteration(mt);
            verify_backward_iteration(mt);
            verify_seeks(mt);
        }
    }

    /* final full sweep */
    verify_forward_iteration(mt);
    verify_backward_iteration(mt);
    verify_seeks(mt);

    /* accounting matches the frozen formula for every stored entry */
    CHECK((size_t)yq_memtable_bytes(mt) == model_bytes());
    CHECK(yq_memtable_size(mt) == model_entries());

    yq_memtable_destroy(mt);
    printf("OK\n");
}

/* ── ordering under reverse-sorted bulk load (the array's worst case) ──── */

static void test_reverse_bulk_order(void) {
    printf("test_reverse_bulk_order... ");

    yq_memtable *mt = yq_memtable_create(64 * 1024 * 1024);
    CHECK(mt != NULL);

    enum { N = 5000 };
    uint8_t vals[N][8];
    for (int i = N - 1; i >= 0; i--) {
        char kbuf[16];
        snprintf(kbuf, sizeof(kbuf), "bk%06d", i);
        for (int b = 0; b < 8; b++) vals[i][b] = (uint8_t)(i * 31 + b);
        yq_slice key = { kbuf, strlen(kbuf) };
        yq_slice val = { vals[i], 8 };
        CHECK(yq_memtable_put(mt, key, val) == YQ_OK);
    }
    CHECK(yq_memtable_size(mt) == N);

    yq_memtable_iter *it = NULL;
    CHECK(yq_memtable_iter_open(mt, &it) == YQ_OK);
    CHECK(yq_memtable_iter_first(it) == YQ_OK);
    for (int i = 0; i < N; i++) {
        CHECK(yq_memtable_iter_valid(it));
        yq_slice k = { 0 }, v = { 0 };
        CHECK(yq_memtable_iter_key(it, &k) == YQ_OK);
        CHECK(yq_memtable_iter_val(it, &v) == YQ_OK);
        char kbuf[16];
        snprintf(kbuf, sizeof(kbuf), "bk%06d", i);
        CHECK(k.size == strlen(kbuf) && memcmp(k.data, kbuf, k.size) == 0);
        CHECK(v.size == 8 && memcmp(v.data, vals[i], 8) == 0);
        if (i + 1 < N) CHECK(yq_memtable_iter_next(it) == YQ_OK);
    }
    CHECK(yq_memtable_iter_next(it) == YQ_ERR_NOTFOUND);

    /* backward pass over the same order */
    CHECK(yq_memtable_iter_last(it) == YQ_OK);
    for (int i = N - 1; i >= 0; i--) {
        yq_slice k = { 0 };
        CHECK(yq_memtable_iter_key(it, &k) == YQ_OK);
        char kbuf[16];
        snprintf(kbuf, sizeof(kbuf), "bk%06d", i);
        CHECK(k.size == strlen(kbuf) && memcmp(k.data, kbuf, k.size) == 0);
        if (i > 0) CHECK(yq_memtable_iter_prev(it) == YQ_OK);
    }
    CHECK(yq_memtable_iter_prev(it) == YQ_ERR_NOTFOUND);
    yq_memtable_iter_close(it);

    yq_memtable_destroy(mt);
    printf("OK\n");
}

/* ── ordering under ascending bulk load (the append+split path) ────────── */

static void test_ascending_bulk_load(void) {
    printf("test_ascending_bulk_load... ");

    yq_memtable *mt = yq_memtable_create(64 * 1024 * 1024);
    CHECK(mt != NULL);

    /*
     * 6000 ascending inserts cross the initial chunk capacity (256) several
     * times, so every append-after-split path runs. This used to be
     * uncovered by the model test (24 keys never split) and by the reverse
     * load (inserts at chunk heads), and a bug there silently destroyed the
     * back half of a chunk on split.
     */
    enum { N = 6000 };
    for (int i = 0; i < N; i++) {
        char kbuf[16];
        snprintf(kbuf, sizeof(kbuf), "ak%06d", i);
        yq_slice key = { kbuf, strlen(kbuf) };
        uint8_t v[8];
        for (int b = 0; b < 8; b++) v[b] = (uint8_t)(i + b);
        yq_slice val = { v, 8 };
        CHECK(yq_memtable_put(mt, key, val) == YQ_OK);
    }
    CHECK(yq_memtable_size(mt) == N);

    /* every key present with the right value */
    for (int i = 0; i < N; i++) {
        char kbuf[16];
        snprintf(kbuf, sizeof(kbuf), "ak%06d", i);
        yq_slice key = { kbuf, strlen(kbuf) };
        yq_slice out = { 0 };
        CHECK(yq_memtable_get(mt, key, &out) == YQ_OK);
        CHECK(out.size == 8);
        uint8_t want[8];
        for (int b = 0; b < 8; b++) want[b] = (uint8_t)(i + b);
        CHECK(memcmp(out.data, want, 8) == 0);
    }

    /* forward iteration hits every key exactly once, in order */
    yq_memtable_iter *it = NULL;
    CHECK(yq_memtable_iter_open(mt, &it) == YQ_OK);
    CHECK(yq_memtable_iter_first(it) == YQ_OK);
    for (int i = 0; i < N; i++) {
        yq_slice k = { 0 };
        CHECK(yq_memtable_iter_valid(it));
        CHECK(yq_memtable_iter_key(it, &k) == YQ_OK);
        char kbuf[16];
        snprintf(kbuf, sizeof(kbuf), "ak%06d", i);
        CHECK(k.size == strlen(kbuf) && memcmp(k.data, kbuf, k.size) == 0);
        if (i + 1 < N) CHECK(yq_memtable_iter_next(it) == YQ_OK);
    }
    CHECK(yq_memtable_iter_next(it) == YQ_ERR_NOTFOUND);

    /* seek to the first key of every chunk-sized stride lands exactly there */
    for (int i = 0; i < N; i += 137) {
        char kbuf[16];
        snprintf(kbuf, sizeof(kbuf), "ak%06d", i);
        yq_slice key = { kbuf, strlen(kbuf) };
        CHECK(yq_memtable_iter_seek(it, key) == YQ_OK);
        yq_slice k = { 0 };
        CHECK(yq_memtable_iter_key(it, &k) == YQ_OK);
        CHECK(k.size == strlen(kbuf) && memcmp(k.data, kbuf, k.size) == 0);
    }
    yq_memtable_iter_close(it);

    yq_memtable_destroy(mt);
    printf("OK\n");
}

/* ── reset() must invalidate cursors, not serve discarded nodes ────────── */

static void test_reset_invalidates(void) {
    printf("test_reset_invalidates... ");

    yq_memtable *mt = yq_memtable_create(1024 * 1024);
    CHECK(mt != NULL);

    yq_slice key = { "gone", 4 };
    yq_slice val = { "x", 1 };
    CHECK(yq_memtable_put(mt, key, val) == YQ_OK);

    yq_memtable_iter *it = NULL;
    CHECK(yq_memtable_iter_open(mt, &it) == YQ_OK);
    yq_slice k = { 0 };
    CHECK(yq_memtable_iter_key(it, &k) == YQ_OK);

    yq_memtable_reset(mt);

    /* Every accessor must refuse the stale cursor instead of returning the
     * bytes the reset discarded. */
    CHECK(yq_memtable_iter_key(it, &k) == YQ_ERR_CURSOR);
    CHECK(yq_memtable_iter_val(it, &k) == YQ_ERR_CURSOR);
    CHECK(yq_memtable_iter_next(it) == YQ_ERR_CURSOR);
    CHECK(yq_memtable_iter_prev(it) == YQ_ERR_CURSOR);
    CHECK(yq_memtable_iter_seek(it, key) != YQ_OK || !yq_memtable_iter_valid(it));
    CHECK(!yq_memtable_iter_valid(it));

    /* Re-positioning after the reset is legal again. */
    CHECK(yq_memtable_iter_first(it) == YQ_ERR_NOTFOUND); /* table is empty */
    yq_slice val2 = { "y", 1 };
    CHECK(yq_memtable_put(mt, key, val2) == YQ_OK);
    CHECK(yq_memtable_iter_first(it) == YQ_OK);
    yq_slice v = { 0 };
    CHECK(yq_memtable_iter_val(it, &v) == YQ_OK);
    CHECK(v.size == 1 && memcmp(v.data, "y", 1) == 0);

    yq_memtable_iter_close(it);
    CHECK(yq_memtable_reserve(mt, 2 * 1024 * 1024) == YQ_OK);
    yq_slice out = { 0 };
    CHECK(yq_memtable_get(mt, key, &out) == YQ_OK);
    CHECK(out.size == 1 && memcmp(out.data, "y", 1) == 0);

    yq_memtable_destroy(mt);
    printf("OK\n");
}

/* ── bulk_load: chunk-boundary sizes, tombstones, empty-table contract ── */

static void check_bulk_run(size_t n) {
    yq_memtable *mt = yq_memtable_create(16 * 1024 * 1024);
    CHECK(mt != NULL);

    /* Per-run storage: the entry array references these bytes until the
     * bulk load has copied them into the arena. */
    char (*keys)[16] = (char (*)[16])malloc(n * sizeof(*keys));
    uint8_t (*vals)[8] = (uint8_t (*)[8])malloc(n * sizeof(*vals));
    yq_bulk_entry *es = (yq_bulk_entry *)malloc(n * sizeof(yq_bulk_entry));
    CHECK(keys && vals && es);

    for (size_t i = 0; i < n; i++) {
        snprintf(keys[i], 16, "bk%06d", (int)i);
        int tomb = ((i % 5) == 3);
        es[i].key.data = keys[i];
        es[i].key.size = strlen(keys[i]);
        if (tomb) {
            es[i].val.data = NULL;
            es[i].val.size = 0;
            es[i].tombstone = 1;
        } else {
            for (int b = 0; b < 8; b++) vals[i][b] = (uint8_t)((int)i * 7 + b);
            es[i].val.data = vals[i];
            es[i].val.size = 8;
            es[i].tombstone = 0;
        }
    }

    CHECK(yq_memtable_bulk_load(mt, es, n) == YQ_OK);
    CHECK((size_t)yq_memtable_size(mt) == n);

    /* point lookups: live entries readable, tombstones invisible */
    for (size_t i = 0; i < n; i++) {
        char kbuf[16];
        snprintf(kbuf, sizeof(kbuf), "bk%06d", (int)i);
        yq_slice key = { kbuf, strlen(kbuf) };
        yq_slice out = { 0 };
        int rc = yq_memtable_get(mt, key, &out);
        if (i % 5 == 3) {
            CHECK(rc == YQ_ERR_NOTFOUND);
        } else {
            CHECK(rc == YQ_OK);
            CHECK(out.size == 8);
            uint8_t want[8];
            for (int b = 0; b < 8; b++) want[b] = (uint8_t)((int)i * 7 + b);
            CHECK(memcmp(out.data, want, 8) == 0);
        }
    }

    /* forward iteration visits exactly the live entries, in order */
    size_t live = 0;
    for (size_t i = 0; i < n; i++) {
        if (i % 5 != 3) live++;
    }

    yq_memtable_iter *it = NULL;
    CHECK(yq_memtable_iter_open(mt, &it) == YQ_OK);
    size_t seen = 0;
    for (size_t i = 0; i < n; i++) {
        if (i % 5 == 3) continue;
        CHECK(yq_memtable_iter_valid(it));
        yq_slice k = { 0 };
        CHECK(yq_memtable_iter_key(it, &k) == YQ_OK);
        char kbuf[16];
        snprintf(kbuf, sizeof(kbuf), "bk%06d", (int)i);
        CHECK(k.size == strlen(kbuf) && memcmp(k.data, kbuf, k.size) == 0);
        seen++;
        int rc = yq_memtable_iter_next(it);
        CHECK(rc == YQ_OK || rc == YQ_ERR_NOTFOUND);
    }
    CHECK(seen == live);
    CHECK(!yq_memtable_iter_valid(it));
    CHECK(yq_memtable_iter_next(it) == YQ_ERR_NOTFOUND);
    yq_memtable_iter_close(it);

    /* accounting: clean per-entry formula */
    size_t want = 0;
    for (size_t i = 0; i < n; i++) {
        want += (i % 5 == 3) ? (8 + 40) : (8 + 8 + 40);
    }
    CHECK(yq_memtable_bytes(mt) == want);

    free(es);
    free(keys);
    free(vals);
    yq_memtable_destroy(mt);
}

static void test_bulk_load(void) {
    printf("test_bulk_load... ");

    /* 256 is the initial chunk capacity, 2048 the split capacity: exercise
     * just under, exactly at, and past each boundary. */
    size_t sizes[] = {1, 2, 255, 256, 257, 511, 2047, 2048, 2049, 5000};
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        check_bulk_run(sizes[s]);
    }

    /* the empty-table contract must be enforced */
    yq_memtable *mt = yq_memtable_create(1024 * 1024);
    CHECK(mt != NULL);
    yq_bulk_entry e;
    memset(&e, 0, sizeof(e));
    e.key.data = "bk000001";
    e.key.size = 8;
    e.val.data = "xyz";
    e.val.size = 3;
    CHECK(yq_memtable_bulk_load(mt, &e, 1) == YQ_OK);
    CHECK(yq_memtable_bulk_load(mt, &e, 1) == YQ_ERR_INVAL);
    yq_slice key = { "bk000001", 8 };
    yq_memtable_del(mt, key);            /* tombstones keep the table busy */
    CHECK(yq_memtable_bulk_load(mt, &e, 1) == YQ_ERR_INVAL);
    yq_memtable_destroy(mt);

    /* invalid keys are rejected, like yq_memtable_put() */
    yq_memtable *mt2 = yq_memtable_create(1024 * 1024);
    CHECK(mt2 != NULL);
    yq_bulk_entry bad;
    memset(&bad, 0, sizeof(bad));
    CHECK(yq_memtable_bulk_load(mt2, &bad, 1) == YQ_ERR_INVAL);
    yq_memtable_destroy(mt2);

    printf("OK\n");
}

/* ── recovery semantics through the real WAL: last write wins ─────────── */

static void test_recover_last_write_wins(void) {
    printf("test_recover_last_write_wins... ");

    remove_recover_files();

    yq_wal *wal = NULL;
    CHECK(yq_wal_open(&wal, "recover_order.db", 4096, YQ_SYNC_NORMAL) == YQ_OK);

    yq_slice k1 = {"k1", 2}, k2 = {"k2", 2}, k3 = {"k3", 2}, k4 = {"k4", 2};
    yq_slice va = {"a", 1}, vb = {"b", 1}, vc = {"c", 1}, vd = {"d", 1};

    /* txn1: k1=a ; txn2: k1=b (overwrite) ; txn3: del k1 (hides both) */
    CHECK(yq_wal_append_begin(wal, 1) == YQ_OK);
    CHECK(yq_wal_append_put(wal, 1, k1, va) == YQ_OK);
    CHECK(yq_wal_append_commit(wal, 1) == YQ_OK);
    CHECK(yq_wal_append_begin(wal, 2) == YQ_OK);
    CHECK(yq_wal_append_put(wal, 2, k1, vb) == YQ_OK);
    CHECK(yq_wal_append_commit(wal, 2) == YQ_OK);
    CHECK(yq_wal_append_begin(wal, 3) == YQ_OK);
    CHECK(yq_wal_append_del(wal, 3, k1) == YQ_OK);
    CHECK(yq_wal_append_commit(wal, 3) == YQ_OK);

    /* txn4: k2=c ; txn6: k2=d (committed later, must win) */
    CHECK(yq_wal_append_begin(wal, 4) == YQ_OK);
    CHECK(yq_wal_append_put(wal, 4, k2, vc) == YQ_OK);
    CHECK(yq_wal_append_commit(wal, 4) == YQ_OK);
    CHECK(yq_wal_append_begin(wal, 5) == YQ_OK);
    CHECK(yq_wal_append_put(wal, 5, k3, vd) == YQ_OK);   /* NO commit */
    CHECK(yq_wal_append_begin(wal, 6) == YQ_OK);
    CHECK(yq_wal_append_put(wal, 6, k2, vd) == YQ_OK);
    CHECK(yq_wal_append_commit(wal, 6) == YQ_OK);

    /* txn7: del k4 (absent -> tombstone) ; txn8: k4=a (revival wins) */
    CHECK(yq_wal_append_begin(wal, 7) == YQ_OK);
    CHECK(yq_wal_append_del(wal, 7, k4) == YQ_OK);
    CHECK(yq_wal_append_commit(wal, 7) == YQ_OK);
    CHECK(yq_wal_append_begin(wal, 8) == YQ_OK);
    CHECK(yq_wal_append_put(wal, 8, k4, va) == YQ_OK);
    CHECK(yq_wal_append_commit(wal, 8) == YQ_OK);

    CHECK(yq_wal_flush(wal) == YQ_OK);
    CHECK(yq_wal_close(wal) == YQ_OK);

    yq_wal *wal2 = NULL;
    CHECK(yq_wal_open(&wal2, "recover_order.db", 4096, YQ_SYNC_NORMAL) == YQ_OK);
    yq_memtable *mt = yq_memtable_create(1024 * 1024);
    CHECK(mt != NULL);
    CHECK(yq_recover(wal2, mt) == YQ_OK);
    CHECK(yq_wal_close(wal2) == YQ_OK);

    yq_slice out = { 0 };
    CHECK(yq_memtable_get(mt, k1, &out) == YQ_ERR_NOTFOUND);  /* deleted */
    CHECK(yq_memtable_get(mt, k2, &out) == YQ_OK);            /* last write */
    CHECK(out.size == 1 && memcmp(out.data, "d", 1) == 0);
    CHECK(yq_memtable_get(mt, k3, &out) == YQ_ERR_NOTFOUND);  /* uncommitted */
    CHECK(yq_memtable_get(mt, k4, &out) == YQ_OK);            /* revived */
    CHECK(out.size == 1 && memcmp(out.data, "a", 1) == 0);

    /* three entries survive: k1 as a tombstone (deleted), k2 and k4 live.
     * k3 was never committed, so it leaves nothing behind. */
    CHECK(yq_memtable_size(mt) == 3);

    yq_memtable_destroy(mt);
    remove_recover_files();
    printf("OK\n");
}

int main(void) {
    printf("=== yq-DB P1/P2 Memtable Model Tests ===\n\n");

    test_model_random();
    test_ascending_bulk_load();
    test_reverse_bulk_order();
    test_reset_invalidates();
    test_bulk_load();
    test_recover_last_write_wins();

    printf("\n=== ALL TESTS PASSED ===\n");
    return 0;
}
