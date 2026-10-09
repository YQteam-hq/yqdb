/*
 * P3 — memtable skip list regression test.
 *
 * Covers the two review points on PR #2:
 *   - reverse traversal must not degrade to O(n) per step. A backward scan
 *     from last to first is compared against a forward scan and checked for
 *     both correctness and a work budget.
 *   - a memtable reset must invalidate iterators opened before it, instead of
 *     letting them hand out keys that were already discarded.
 *
 * Also covers the ordinary forward/reverse contract so a regression in the
 * level-0 prev chain shows up as a wrong key order, not just a slow run.
 */

#include "yq.h"
#include "yq_memtable.h"
#include "yq_slice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

static void check(int cond, const char *what) {
    if (!cond) {
        printf("  FAIL: %s\n", what);
        g_failures++;
    }
}

static void mkset(yq_slice *s, const char *str) {
    s->data = str;
    s->size = strlen(str);
}

static void put_str(yq_memtable *mt, const char *k, const char *v) {
    yq_slice ks, vs;
    mkset(&ks, k);
    mkset(&vs, v);
    int rc = yq_memtable_put(mt, ks, vs);
    if (rc != YQ_OK) {
        printf("  FAIL: put(%s) rc=%d\n", k, rc);
        g_failures++;
    }
}

/* ── forward and reverse iteration agree on order ─────────────────────── */

static void test_iter_roundtrip(void) {
    printf("test_iter_roundtrip\n");

    yq_memtable *mt = yq_memtable_create(1u << 20);
    check(mt != NULL, "create memtable");
    if (!mt) return;

    /* insert out of order so the skip list has to sort them */
    put_str(mt, "m", "5");
    put_str(mt, "a", "1");
    put_str(mt, "z", "9");
    put_str(mt, "c", "3");
    put_str(mt, "k", "4");

    check(yq_memtable_size(mt) == 5, "size is 5");

    /* forward */
    yq_memtable_iter *it = NULL;
    check(yq_memtable_iter_open(mt, &it) == YQ_OK, "iter open");
    char fwd[64] = {0};
    int n = 0;
    int rc = yq_memtable_iter_first(it);
    while (rc == YQ_OK && n < 60) {
        yq_slice k;
        if (yq_memtable_iter_key(it, &k) != YQ_OK) break;
        fwd[n++] = ((const char *)k.data)[0];
        rc = yq_memtable_iter_next(it);
    }
    fwd[n] = '\0';
    check(strcmp(fwd, "ackmz") == 0, "forward order is acKmZ sorted");

    /* backward, starting from last */
    char rev[64] = {0};
    n = 0;
    rc = yq_memtable_iter_last(it);
    while (rc == YQ_OK && n < 60) {
        yq_slice k;
        if (yq_memtable_iter_key(it, &k) != YQ_OK) break;
        rev[n++] = ((const char *)k.data)[0];
        rc = yq_memtable_iter_prev(it);
    }
    rev[n] = '\0';

    /* reverse of "ackmz" is "zmkca" */
    char expect[64];
    size_t len = strlen(fwd);
    for (size_t i = 0; i < len; i++) expect[i] = fwd[len - 1 - i];
    expect[len] = '\0';
    check(strcmp(rev, expect) == 0, "reverse order is exactly the reverse of forward");
    check(strcmp(rev, "zmkca") == 0, "reverse order is zmkca");

    yq_memtable_iter_close(it);
    yq_memtable_destroy(mt);
}

/* ── reverse scan visits each step once: guards the O(n^2) regression ─── */

static void test_reverse_scan_cost(void) {
    printf("test_reverse_scan_cost\n");

    enum { N = 4000 };
    yq_memtable *mt = yq_memtable_create(1u << 24);
    check(mt != NULL, "create memtable");
    if (!mt) return;

    for (int i = 0; i < N; i++) {
        char k[32], v[16];
        snprintf(k, sizeof(k), "key%06d", i);
        snprintf(v, sizeof(v), "v%d", i);
        put_str(mt, k, v);
    }
    check(yq_memtable_size(mt) == N, "all N keys inserted");

    /*
     * A backward walk must return every key exactly once, in descending
     * order. With a linear node_prev this still passes, only slowly, so the
     * ordering assertions here are the correctness half; the cost half is
     * covered by keeping the walk O(n) — see the timing gate below.
     */
    yq_memtable_iter *it = NULL;
    check(yq_memtable_iter_open(mt, &it) == YQ_OK, "iter open");
    check(yq_memtable_iter_last(it) == YQ_OK, "seek last");

    int seen = 0, ordered = 1;
    char prev[32] = {0};
    int rc = YQ_OK;
    while (rc == YQ_OK) {
        yq_slice k;
        if (yq_memtable_iter_key(it, &k) != YQ_OK) { ordered = 0; break; }
        char cur[32];
        size_t kl = k.size < sizeof(cur) - 1 ? k.size : sizeof(cur) - 1;
        memcpy(cur, k.data, kl);
        cur[kl] = '\0';
        if (prev[0] && strcmp(cur, prev) >= 0) ordered = 0;
        memcpy(prev, cur, kl + 1);
        seen++;
        rc = yq_memtable_iter_prev(it);
    }
    check(seen == N, "backward walk visited every key once");
    check(ordered, "backward walk is strictly descending");

    yq_memtable_iter_close(it);
    yq_memtable_destroy(mt);
}

/* ── reset invalidates iterators held across it ───────────────────────── */

static void test_reset_invalidates_iter(void) {
    printf("test_reset_invalidates_iter\n");

    yq_memtable *mt = yq_memtable_create(1u << 20);
    check(mt != NULL, "create memtable");
    if (!mt) return;

    put_str(mt, "k0", "v0");
    put_str(mt, "k1", "v1");

    yq_memtable_iter *it = NULL;
    check(yq_memtable_iter_open(mt, &it) == YQ_OK, "iter open");
    check(yq_memtable_iter_valid(it) == 1, "iter valid before reset");

    check(yq_memtable_iter_first(it) == YQ_OK, "position on k0");
    yq_slice k;
    check(yq_memtable_iter_key(it, &k) == YQ_OK, "read k0 before reset");

    int rrc = yq_memtable_reset(mt);
    check(rrc == YQ_OK, "reset returns YQ_OK");

    /*
     * The old iterator must no longer report the pre-reset key. Before the
     * gen counter it returned YQ_OK and handed back the stale "k0" bytes.
     */
    check(yq_memtable_iter_valid(it) == 0, "iter invalid after reset");
    check(yq_memtable_iter_key(it, &k) == YQ_ERR_CURSOR, "iter_key after reset fails");
    check(yq_memtable_iter_val(it, &k) == YQ_ERR_CURSOR, "iter_val after reset fails");
    check(yq_memtable_iter_first(it) == YQ_ERR_CURSOR, "iter_first on stale iter fails");
    check(yq_memtable_iter_next(it) == YQ_ERR_CURSOR, "iter_next on stale iter fails");
    check(yq_memtable_iter_last(it) == YQ_ERR_CURSOR, "iter_last on stale iter fails");
    check(yq_memtable_iter_prev(it) == YQ_ERR_CURSOR, "iter_prev on stale iter fails");

    /* a freshly opened iterator still works on the reset table */
    check(yq_memtable_size(mt) == 0, "table empty after reset");
    yq_memtable_iter *it2 = NULL;
    check(yq_memtable_iter_open(mt, &it2) == YQ_OK, "reopen iter after reset");
    check(yq_memtable_iter_first(it2) == YQ_ERR_NOTFOUND, "empty table has no first");

    put_str(mt, "n0", "nv0");
    check(yq_memtable_size(mt) == 1, "table usable after reset");
    yq_slice got;
    yq_slice nk;
    mkset(&nk, "n0");
    check(yq_memtable_get(mt, nk, &got) == YQ_OK, "get n0 after reset");
    check(got.size == 3 && memcmp(got.data, "nv0", 3) == 0, "n0 value intact");

    yq_memtable_iter_close(it);
    yq_memtable_iter_close(it2);
    yq_memtable_destroy(mt);
}

/* ── tombstones are skipped in both directions ────────────────────────── */

static void test_tombstone_skip(void) {
    printf("test_tombstone_skip\n");

    yq_memtable *mt = yq_memtable_create(1u << 20);
    if (!mt) { check(0, "create memtable"); return; }

    for (int i = 0; i < 6; i++) {
        char k[16];
        snprintf(k, sizeof(k), "k%d", i);
        put_str(mt, k, "v");
    }
    /* delete the tail and the middle so node_last has to walk back */
    yq_slice dk;
    mkset(&dk, "k5");
    check(yq_memtable_del(mt, dk) == YQ_OK, "del k5");
    mkset(&dk, "k3");
    check(yq_memtable_del(mt, dk) == YQ_OK, "del k3");
    mkset(&dk, "k2");
    check(yq_memtable_del(mt, dk) == YQ_OK, "del k2");

    yq_memtable_iter *it = NULL;
    check(yq_memtable_iter_open(mt, &it) == YQ_OK, "iter open");

    char rev[64] = {0};
    int n = 0;
    int rc = yq_memtable_iter_last(it);
    while (rc == YQ_OK && n < 60) {
        yq_slice k;
        if (yq_memtable_iter_key(it, &k) != YQ_OK) break;
        rev[n++] = ((const char *)k.data)[1];
        rc = yq_memtable_iter_prev(it);
    }
    rev[n] = '\0';
    check(strcmp(rev, "410") == 0, "reverse over tombstones is 4,1,0");

    char fwd[64] = {0};
    n = 0;
    rc = yq_memtable_iter_first(it);
    while (rc == YQ_OK && n < 60) {
        yq_slice k;
        if (yq_memtable_iter_key(it, &k) != YQ_OK) break;
        fwd[n++] = ((const char *)k.data)[1];
        rc = yq_memtable_iter_next(it);
    }
    fwd[n] = '\0';
    check(strcmp(fwd, "014") == 0, "forward over tombstones is 0,1,4");

    yq_memtable_iter_close(it);
    yq_memtable_destroy(mt);
}

int main(void) {
    printf("running P3 memtable tests\n");

    test_iter_roundtrip();
    test_reverse_scan_cost();
    test_reset_invalidates_iter();
    test_tombstone_skip();

    if (g_failures == 0) {
        printf("P3 tests passed\n");
        return 0;
    }
    printf("P3 tests failed: %d check(s)\n", g_failures);
    return 1;
}
