/*
 * Memtable cursor tests.
 *
 * The cursor used to walk tombstones one slot at a time: first/last/next/prev
 * and the seek helper all advanced linearly, which made a seek on a large
 * memtable O(n) and a range scan that seeks per step O(n^2). The cursor now
 * skips tombstone runs with a block summary, so this file pins down both the
 * result set and the ordering that the fast paths must preserve.
 *
 * Coverage:
 *   - forward and backward scans visit exactly the live set, in order, when
 *     tombstones are interleaved with live entries (including runs longer
 *     than the internal linear-probe window);
 *   - yq_memtable_iter_seek() matches a linear lower-bound scan for targets
 *     that are present, absent, before the first key and past the last key;
 *   - a tombstone-only span at the head/tail of the array is skipped;
 *   - a resurrected key (put -> del -> put) is live again and lands in the
 *     right position.
 *
 * Results are cross-checked against a reference model kept in a plain array,
 * not against the library itself.
 */

#include "yq_memtable.h"
#include "yq_slice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NKEYS   4000
#define NGAP    8      /* keys between data points, to leave tombstone runs */
#define LONG_RUN 40    /* longer than YQ_MT_LINEAR_PROBE (16) */

static int g_failures = 0;

static void check(int cond, const char *what) {
    if (!cond) {
        printf("  FAIL: %s\n", what);
        g_failures++;
    }
}

static void mkkey(char *buf, size_t cap, int id) {
    snprintf(buf, cap, "key%06d", id);
}

static void set_str(yq_slice *s, const char *p) {
    s->data = p;
    s->size = strlen(p);
}

/* ── reference model ─────────────────────────────────────────────────── */

/* live[id] is 1 when key id should be visible through the cursor. */
static unsigned char g_live[NKEYS];
static char          g_val[NKEYS][24];

static size_t model_live_total(void) {
    size_t n = 0;
    for (int i = 0; i < NKEYS; i++) if (g_live[i]) n++;
    return n;
}

/* first key >= target among live keys, written to out (empty when none) */
static void model_lower_bound(const char *target, char *out, size_t cap) {
    out[0] = '\0';
    for (int i = 0; i < NKEYS; i++) {
        if (!g_live[i]) continue;
        char cand[32];
        mkkey(cand, sizeof(cand), i);
        if (strcmp(cand, target) >= 0) { snprintf(out, cap, "%s", cand); return; }
    }
}

/* ── dataset ─────────────────────────────────────────────────────────── */

/*
 * Build a memtable where every NGAP-th key is live and the rest of the array
 * is tombstones, so a scan has to skip runs at least LONG_RUN long. Then
 * delete a couple of live keys to mix short and long runs.
 */
static yq_memtable *build(void) {
    yq_memtable *mt = yq_memtable_create(64u << 20);
    if (!mt) return NULL;

    char kb[32];

    /* Create all keys live first, then delete the gaps: this produces
     * tombstones interleaved with live entries in sorted order. */
    for (int i = 0; i < NKEYS; i++) {
        mkkey(kb, sizeof(kb), i);
        snprintf(g_val[i], sizeof(g_val[i]), "v%d", i);
        yq_slice k, v;
        set_str(&k, kb);
        v.data = g_val[i];
        v.size = strlen(g_val[i]);
        if (yq_memtable_put(mt, k, v) != YQ_OK) return NULL;
        g_live[i] = 1;
    }

    /* Delete everything except a regular lattice, then delete a slab of
     * LONG_RUN consecutive survivors to force a long tombstone run. */
    for (int i = 0; i < NKEYS; i++) {
        int keep = (i % NGAP) == 0;
        if (!keep) {
            mkkey(kb, sizeof(kb), i);
            yq_slice k;
            set_str(&k, kb);
            yq_memtable_del(mt, k);
            g_live[i] = 0;
        }
    }

    /* Carve a LONG_RUN-long hole out of the lattice. */
    int carved = 0;
    for (int i = 0; i < NKEYS && carved < LONG_RUN; i++) {
        if (g_live[i]) {
            mkkey(kb, sizeof(kb), i);
            yq_slice k;
            set_str(&k, kb);
            yq_memtable_del(mt, k);
            g_live[i] = 0;
            carved++;
        }
    }

    /* Resurrect one deleted key: put -> del -> put must end up live, and the
     * model has to record the new value. */
    for (int i = 0; i < NKEYS; i++) {
        if (!g_live[i]) {
            mkkey(kb, sizeof(kb), i);
            yq_slice k, v;
            set_str(&k, kb);
            set_str(&v, "resurrected");
            if (yq_memtable_put(mt, k, v) != YQ_OK) return NULL;
            g_live[i] = 1;
            snprintf(g_val[i], sizeof(g_val[i]), "resurrected");
            break;
        }
    }

    return mt;
}

/* ── tests ───────────────────────────────────────────────────────────── */

static void test_forward_scan(yq_memtable *mt) {
    printf("test_forward_scan\n");

    yq_memtable_iter *it = NULL;
    check(yq_memtable_iter_open(mt, &it) == YQ_OK, "iter_open");

    size_t seen = 0;
    int ordered = 1;
    char prev[32] = {0};

    for (int rc = yq_memtable_iter_first(it); rc == YQ_OK; rc = yq_memtable_iter_next(it)) {
        yq_slice k, v;
        if (yq_memtable_iter_key(it, &k) != YQ_OK) { check(0, "iter_key"); break; }
        if (yq_memtable_iter_val(it, &v) != YQ_OK) { check(0, "iter_val"); break; }

        char cur[32];
        snprintf(cur, sizeof(cur), "%.*s", (int)k.size, (const char *)k.data);

        if (seen > 0 && strcmp(prev, cur) >= 0) ordered = 0;
        snprintf(prev, sizeof(prev), "%s", cur);

        int id = atoi(cur + 3);
        if (id < 0 || id >= NKEYS) { check(0, "forward scan produced an out-of-range key"); break; }
        if (!g_live[id]) { check(0, "forward scan produced a deleted key"); break; }

        const char *want = g_val[id];
        if (v.size != strlen(want) || memcmp(v.data, want, v.size) != 0) {
            check(0, "forward scan value mismatch"); break;
        }
        seen++;
    }

    if (seen != model_live_total()) printf("  (forward saw %zu, model has %zu)\n", seen, model_live_total());
    check(seen == model_live_total(), "forward scan visits exactly the live set");
    check(ordered, "forward scan is strictly increasing");
    yq_memtable_iter_close(it);
}

static void test_backward_scan(yq_memtable *mt) {
    printf("test_backward_scan\n");

    yq_memtable_iter *it = NULL;
    yq_memtable_iter_open(mt, &it);

    size_t seen = 0;
    int ordered = 1;
    char prev[32] = {0};

    for (int rc = yq_memtable_iter_last(it); rc == YQ_OK; rc = yq_memtable_iter_prev(it)) {
        yq_slice k;
        if (yq_memtable_iter_key(it, &k) != YQ_OK) { check(0, "iter_key"); break; }

        char cur[32];
        snprintf(cur, sizeof(cur), "%.*s", (int)k.size, (const char *)k.data);

        if (seen > 0 && strcmp(prev, cur) <= 0) ordered = 0;
        snprintf(prev, sizeof(prev), "%s", cur);

        int id = atoi(cur + 3);
        if (id < 0 || id >= NKEYS) { check(0, "backward scan produced an out-of-range key"); break; }
        if (!g_live[id]) { check(0, "backward scan produced a deleted key"); break; }
        seen++;
    }

    if (seen != model_live_total()) printf("  (backward saw %zu, model has %zu)\n", seen, model_live_total());
    check(seen == model_live_total(), "backward scan visits exactly the live set");
    check(ordered, "backward scan is strictly decreasing");
    yq_memtable_iter_close(it);
}

static void test_seek(yq_memtable *mt) {
    printf("test_seek\n");

    int mismatches = 0;
    int probes = 0;

    /* Cover: every key, keys just past a live one, before the head, past the
     * tail, and inside the carved hole. */
    for (int base = 0; base < NKEYS; base += 7) {
        for (int delta = 0; delta <= 2; delta++) {
            char target[32], expected[32];
            mkkey(target, sizeof(target), base + delta);
            model_lower_bound(target, expected, sizeof(expected));

            yq_slice tk;
            set_str(&tk, target);

            yq_memtable_iter *it = NULL;
            yq_memtable_iter_open(mt, &it);
            int rc = yq_memtable_iter_seek(it, tk);

            char got[32] = {0};
            if (rc == YQ_OK) {
                yq_slice k;
                if (yq_memtable_iter_key(it, &k) == YQ_OK) {
                    snprintf(got, sizeof(got), "%.*s", (int)k.size, (const char *)k.data);
                }
            }
            yq_memtable_iter_close(it);

            if (strcmp(expected, got) != 0) {
                if (mismatches < 3) {
                    printf("  seek mismatch: target=%s expected=%s got=%s (rc=%d)\n",
                           target, expected, got, rc);
                }
                mismatches++;
            }
            probes++;
        }
    }

    /* Targets outside the whole key range. */
    {
        const char *outside[] = { "aaa", "zzz", "key", "key0", "zzzzzzzz" };
        for (size_t i = 0; i < sizeof(outside) / sizeof(outside[0]); i++) {
            char expected[32];
            model_lower_bound(outside[i], expected, sizeof(expected));

            yq_slice tk;
            set_str(&tk, outside[i]);

            yq_memtable_iter *it = NULL;
            yq_memtable_iter_open(mt, &it);
            int rc = yq_memtable_iter_seek(it, tk);
            char got[32] = {0};
            if (rc == YQ_OK) {
                yq_slice k;
                if (yq_memtable_iter_key(it, &k) == YQ_OK) {
                    snprintf(got, sizeof(got), "%.*s", (int)k.size, (const char *)k.data);
                }
            }
            yq_memtable_iter_close(it);

            if (strcmp(expected, got) != 0) {
                printf("  seek mismatch outside range: target=%s expected=%s got=%s\n",
                       outside[i], expected, got);
                mismatches++;
            }
            probes++;
        }
    }

    printf("  %d seek probes, %d mismatches\n", probes, mismatches);
    check(mismatches == 0, "iter_seek matches the linear lower bound everywhere");
}

static void test_edges(void) {
    printf("test_edges\n");

    /* Empty memtable: every positioning call reports "not found". */
    {
        yq_memtable *mt = yq_memtable_create(1u << 20);
        yq_memtable_iter *it = NULL;
        yq_memtable_iter_open(mt, &it);
        check(yq_memtable_iter_first(it) == YQ_ERR_NOTFOUND, "empty: first");
        check(yq_memtable_iter_last(it) == YQ_ERR_NOTFOUND, "empty: last");
        yq_slice k;
        set_str(&k, "anything");
        check(yq_memtable_iter_seek(it, k) == YQ_ERR_NOTFOUND, "empty: seek");
        check(yq_memtable_iter_valid(it) == 0, "empty: invalid");
        yq_memtable_iter_close(it);
        yq_memtable_destroy(mt);
    }

    /* Head and tail tombstone runs must be skipped in both directions. */
    {
        yq_memtable *mt = yq_memtable_create(1u << 20);
        char kb[32];
        const int total = 200;

        for (int i = 0; i < total; i++) {
            mkkey(kb, sizeof(kb), i);
            yq_slice k, v;
            set_str(&k, kb);
            set_str(&v, "x");
            yq_memtable_put(mt, k, v);
        }
        /* Delete the first 60 and the last 60, keep the middle 80. */
        for (int i = 0; i < 60; i++) {
            mkkey(kb, sizeof(kb), i);
            yq_slice k;
            set_str(&k, kb);
            yq_memtable_del(mt, k);
        }
        for (int i = total - 60; i < total; i++) {
            mkkey(kb, sizeof(kb), i);
            yq_slice k;
            set_str(&k, kb);
            yq_memtable_del(mt, k);
        }

        yq_memtable_iter *it = NULL;
        yq_memtable_iter_open(mt, &it);

        check(yq_memtable_iter_first(it) == YQ_OK, "head run: first found");
        yq_slice k;
        yq_memtable_iter_key(it, &k);
        char first[32];
        snprintf(first, sizeof(first), "%.*s", (int)k.size, (const char *)k.data);
        check(strcmp(first, "key000060") == 0, "head tombstone run skipped");

        check(yq_memtable_iter_last(it) == YQ_OK, "tail run: last found");
        yq_memtable_iter_key(it, &k);
        char last[32];
        snprintf(last, sizeof(last), "%.*s", (int)k.size, (const char *)k.data);
        check(strcmp(last, "key000139") == 0, "tail tombstone run skipped");

        size_t count = 0;
        for (int rc = yq_memtable_iter_first(it); rc == YQ_OK; rc = yq_memtable_iter_next(it)) count++;
        check(count == 80, "head+tail run: exactly the middle 80 are visible");

        yq_memtable_iter_close(it);
        yq_memtable_destroy(mt);
    }

    /* All tombstones: the cursor reports "not found" and never yields a key. */
    {
        yq_memtable *mt = yq_memtable_create(1u << 20);
        char kb[32];
        for (int i = 0; i < 100; i++) {
            mkkey(kb, sizeof(kb), i);
            yq_slice k, v;
            set_str(&k, kb);
            set_str(&v, "x");
            yq_memtable_put(mt, k, v);
        }
        for (int i = 0; i < 100; i++) {
            mkkey(kb, sizeof(kb), i);
            yq_slice k;
            set_str(&k, kb);
            yq_memtable_del(mt, k);
        }
        yq_memtable_iter *it = NULL;
        yq_memtable_iter_open(mt, &it);
        check(yq_memtable_iter_first(it) == YQ_ERR_NOTFOUND, "all-dead: first");
        check(yq_memtable_iter_last(it) == YQ_ERR_NOTFOUND, "all-dead: last");
        check(yq_memtable_iter_valid(it) == 0, "all-dead: invalid");
        yq_memtable_iter_close(it);
        yq_memtable_destroy(mt);
    }
}

int main(void) {
    printf("running memtable cursor tests\n");

    yq_memtable *mt = build();
    if (!mt) { printf("  FAIL: could not build the memtable\n"); return 1; }

    printf("  dataset: %d live keys in a %d-entry array\n",
           (int)model_live_total(), yq_memtable_size(mt));

    test_forward_scan(mt);
    test_backward_scan(mt);
    test_seek(mt);

    yq_memtable_destroy(mt);

    test_edges();

    if (g_failures == 0) {
        printf("memtable cursor tests passed\n");
        return 0;
    }
    printf("memtable cursor tests failed: %d check(s)\n", g_failures);
    return 1;
}
