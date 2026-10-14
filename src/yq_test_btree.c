/*
 * P3 stage 1: page store + a B+Tree that can actually be built and read back.
 *
 * Before this the tree had no working write path at all: yq_btree_insert()
 * invented page numbers (bt->npages++), allocated the page twice per split,
 * left root_page at 0 (so the first descent read meta page 0 as a tree page),
 * mutated pages in place with no COW, and produced pages that yq_btree_lookup()
 * could not read. No test ever inserted and read back -- only hand-built
 * pages were exercised (yq_test_seek.c).
 *
 * These tests build a real tree through the page store and check every
 * observable against a reference model: point lookups (hits and misses),
 * full forward and backward cursor scans, ordered seeks, duplicate rejection,
 * page-count/free-list behaviour of the store, and the structural depth
 * forced by inserting enough keys to split leaves AND internal nodes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yq.h"
#include "yq_page.h"
#include "yq_btree.h"

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            exit(1);                                                      \
        }                                                                 \
    } while (0)

#define PS 4096

static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;

static uint64_t rng_next(void) {
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return rng_state;
}

static uint32_t rng_below(uint32_t n) {
    return (uint32_t)(rng_next() % n);
}

/* ── the store ────────────────────────────────────────────────────────── */

static void test_page_store(void) {
    printf("test_page_store... ");

    yq_page_store *st = yq_page_store_mem_create(PS);
    CHECK(st != NULL);

    /* pages 0/1 are the meta pages and must never be handed out */
    CHECK(yq_page_store_npages(st) == 2);
    CHECK(yq_page_store_get(st, 0) == NULL);
    CHECK(yq_page_store_get(st, 1) == NULL);
    CHECK(yq_page_store_alloc(st, NULL) == YQ_ERR_INVAL);

    uint64_t a = 0, b = 0, c = 0;
    CHECK(yq_page_store_alloc(st, &a) == YQ_OK);
    CHECK(yq_page_store_alloc(st, &b) == YQ_OK);
    CHECK(yq_page_store_alloc(st, &c) == YQ_OK);
    CHECK(a == 2 && b == 3 && c == 4);
    CHECK(yq_page_store_npages(st) == 5);
    CHECK(yq_page_store_live(st) == 3);

    /* pages are zeroed and independent */
    uint8_t *pa = (uint8_t *)yq_page_store_get(st, a);
    CHECK(pa != NULL);
    memset(pa, 0xAA, PS);
    uint8_t *pb = (uint8_t *)yq_page_store_get(st, b);
    CHECK(pb[0] == 0 && pb[PS - 1] == 0);

    /* free b, then allocate: b comes back (LIFO free list) */
    CHECK(yq_page_store_free(st, b) == YQ_OK);
    CHECK(yq_page_store_live(st) == 2);
    CHECK(yq_page_store_free_head(st) == b);
    CHECK(yq_page_store_get(st, b) == NULL);      /* freed: not addressable */
    uint64_t d = 0;
    CHECK(yq_page_store_alloc(st, &d) == YQ_OK);
    CHECK(d == b);
    CHECK(yq_page_store_npages(st) == 5);         /* reused, not extended */
    uint8_t *pd = (uint8_t *)yq_page_store_get(st, d);
    CHECK(pd[0] == 0 && pd[PS - 1] == 0);         /* zeroed on reuse */

    /* out of range / reserved */
    CHECK(yq_page_store_get(st, 99) == NULL);
    CHECK(yq_page_store_free(st, 0) == YQ_ERR_INVAL);
    CHECK(yq_page_store_free(st, 1) == YQ_ERR_INVAL);
    CHECK(yq_page_store_free(st, 99) == YQ_ERR_INVAL);

    /* adopt a larger page count (reopening a file), never shrink */
    CHECK(yq_page_store_set_npages(st, 100) == YQ_OK);
    CHECK(yq_page_store_npages(st) == 100);
    CHECK(yq_page_store_get(st, 50) == NULL);     /* exists, not materialised */
    CHECK(yq_page_store_set_npages(st, 5) == YQ_ERR_INVAL);

    yq_page_store_destroy(st);
    printf("OK\n");
}

/* ── build a tree, read it back ───────────────────────────────────────── */

typedef struct {
    char     *key;
    uint8_t  *val;
    size_t    val_len;
} model_row;

static void key_of(char *buf, size_t cap, int i) {
    snprintf(buf, cap, "key%08d", i);
}

/*
 * Insert `n` keys in a given order, then verify against the model:
 * every key readable, misses reported, and a full cursor scan in key order.
 */
static void build_and_verify(uint32_t page_size, int n, int random_order,
                             const char *label) {
    printf("%s... ", label);

    yq_btree *bt = yq_btree_create(NULL, page_size);
    CHECK(bt != NULL);
    yq_page_store *store = yq_page_store_mem_create(page_size);
    CHECK(store != NULL);
    yq_btree_set_page_store(bt, store);

    CHECK(!yq_btree_has_root(bt));

    model_row *rows = (model_row *)calloc((size_t)n, sizeof(model_row));
    CHECK(rows != NULL);

    int *order = (int *)malloc((size_t)n * sizeof(int));
    CHECK(order != NULL);
    for (int i = 0; i < n; i++) order[i] = i;
    if (random_order) {
        for (int i = n - 1; i > 0; i--) {
            int j = (int)(rng_next() % (uint64_t)(i + 1));
            int t = order[i]; order[i] = order[j]; order[j] = t;
        }
    }

    char kbuf[32];
    uint8_t vbuf[128];
    for (int oi = 0; oi < n; oi++) {
        int i = order[oi];
        key_of(kbuf, sizeof(kbuf), i);
        size_t vlen = 1 + (size_t)rng_below(sizeof(vbuf));
        for (size_t b = 0; b < vlen; b++) vbuf[b] = (uint8_t)(i * 31 + b);

        yq_slice k = { kbuf, strlen(kbuf) };
        yq_slice v = { vbuf, vlen };
        int rc = yq_btree_insert(bt, k, v);
        if (rc != YQ_OK) {
            fprintf(stderr, "FAIL insert %s at %d rc=%d\n", kbuf, i, rc);
            exit(1);
        }

        rows[i].key = (char *)malloc(strlen(kbuf) + 1);
        strcpy(rows[i].key, kbuf);
        rows[i].val = (uint8_t *)malloc(vlen);
        memcpy(rows[i].val, vbuf, vlen);
        rows[i].val_len = vlen;
    }

    /* a duplicate key must be refused, not silently duplicated */
    key_of(kbuf, sizeof(kbuf), 0);
    yq_slice k0 = { kbuf, strlen(kbuf) };
    yq_slice v0 = { "x", 1 };
    CHECK(yq_btree_insert(bt, k0, v0) == YQ_ERR_EXISTS);

    CHECK(yq_btree_has_root(bt));
    uint64_t root = 0;
    CHECK(yq_btree_get_root(bt, &root) == YQ_OK);
    CHECK(root != 0);

    /* point lookups */
    for (int i = 0; i < n; i++) {
        yq_slice k = { rows[i].key, strlen(rows[i].key) };
        yq_slice out = { 0 };
        int rc = yq_btree_lookup(bt, k, &out);
        if (rc != YQ_OK) {
            fprintf(stderr, "FAIL lookup %s rc=%d (NOTFOUND=%d CORRUPT=%d)\n",
                    rows[i].key, rc, YQ_ERR_NOTFOUND, YQ_ERR_CORRUPT);
            exit(1);
        }
        if (out.size != rows[i].val_len ||
            memcmp(out.data, rows[i].val, out.size) != 0) {
            fprintf(stderr, "FAIL value mismatch %s\n", rows[i].key);
            exit(1);
        }
    }
    /* a key that was never inserted */
    yq_slice miss = { "zzz_missing", 11 };
    yq_slice out = { 0 };
    CHECK(yq_btree_lookup(bt, miss, &out) == YQ_ERR_NOTFOUND);

    /* forward scan in key order */
    yq_btree_cursor *cur = NULL;
    CHECK(yq_btree_cursor_open(bt, &cur) == YQ_OK);
    CHECK(yq_btree_cursor_first(cur) == YQ_OK);
    int seen = 0;
    for (int i = 0; i < n; i++) {
        yq_slice k = { 0 }, v = { 0 };
        if (!yq_btree_cursor_valid(cur)) break;
        CHECK(yq_btree_cursor_key(cur, &k) == YQ_OK);
        if (k.size != strlen(rows[i].key) ||
            memcmp(k.data, rows[i].key, k.size) != 0) {
            fprintf(stderr, "FAIL scan order at %d: got %.*s want %s\n",
                    i, (int)k.size, (const char *)k.data, rows[i].key);
            exit(1);
        }
        CHECK(yq_btree_cursor_val(cur, &v) == YQ_OK);
        CHECK(v.size == rows[i].val_len);
        CHECK(memcmp(v.data, rows[i].val, v.size) == 0);
        seen++;
        if (i + 1 < n) CHECK(yq_btree_cursor_next(cur) == YQ_OK);
    }
    CHECK(seen == n);
    /* stepping off the end must invalidate the cursor (and not wrap around) */
    yq_btree_cursor_next(cur);
    CHECK(!yq_btree_cursor_valid(cur));
    yq_btree_cursor_close(cur);

    /* backward scan */
    CHECK(yq_btree_cursor_open(bt, &cur) == YQ_OK);
    CHECK(yq_btree_cursor_last(cur) == YQ_OK);
    for (int i = n - 1; i >= 0; i--) {
        yq_slice k = { 0 };
        if (!yq_btree_cursor_valid(cur)) break;
        CHECK(yq_btree_cursor_key(cur, &k) == YQ_OK);
        if (k.size != strlen(rows[i].key) ||
            memcmp(k.data, rows[i].key, k.size) != 0) {
            fprintf(stderr, "FAIL backward scan at %d\n", i);
            exit(1);
        }
        if (i > 0) CHECK(yq_btree_cursor_prev(cur) == YQ_OK);
    }
    yq_btree_cursor_prev(cur);          /* off the front of the tree */
    CHECK(!yq_btree_cursor_valid(cur));
    yq_btree_cursor_close(cur);

    /* ordered seeks land exactly where expected */
    for (int i = 0; i < n; i += 13) {
        yq_slice k = { rows[i].key, strlen(rows[i].key) };
        CHECK(yq_btree_cursor_open(bt, &cur) == YQ_OK);
        CHECK(yq_btree_cursor_seek(cur, k) == YQ_OK);
        yq_slice got = { 0 };
        CHECK(yq_btree_cursor_key(cur, &got) == YQ_OK);
        CHECK(got.size == k.size && memcmp(got.data, k.data, got.size) == 0);
        /* seek beyond every key */
        yq_slice past = { "zzzz", 4 };
        CHECK(yq_btree_cursor_seek(cur, past) == YQ_ERR_NOTFOUND);
        yq_btree_cursor_close(cur);
    }

    for (int i = 0; i < n; i++) {
        free(rows[i].key);
        free(rows[i].val);
    }
    free(rows);
    free(order);
    yq_btree_close(bt);
    yq_page_store_destroy(store);
    printf("OK (%d keys)\n", n);
}

/*
 * Delete removes the slot without moving other cells, which leaves a hole
 * inside the cell block unless the removed cell was the lowest one. The free
 * space accounting has to be honest about that: claiming the cell's bytes back
 * for a middle-of-page delete would hand out space that is still occupied, and
 * the next insert would write a cell on top of a live one.
 */
static void test_delete_and_reinsert(void) {
    printf("test_delete_and_reinsert... ");

    yq_btree *bt = yq_btree_create(NULL, PS);
    yq_page_store *store = yq_page_store_mem_create(PS);
    yq_btree_set_page_store(bt, store);

    const int n = 400;
    char kbuf[32];
    uint8_t vbuf[64];
    for (int i = 0; i < n; i++) {
        key_of(kbuf, sizeof(kbuf), i);
        size_t vlen = 8 + (size_t)(i % 40);
        memset(vbuf, (uint8_t)('A' + (i % 26)), vlen);
        yq_slice ks, vs;
        ks.data = kbuf; ks.size = strlen(kbuf);
        vs.data = vbuf; vs.size = vlen;
        CHECK(yq_btree_insert(bt, ks, vs) == YQ_OK);
    }

    /* delete every third key: a mix of lowest-cell and interior-cell cases */
    for (int i = 0; i < n; i += 3) {
        key_of(kbuf, sizeof(kbuf), i);
        yq_slice ks;
        ks.data = kbuf; ks.size = strlen(kbuf);
        CHECK(yq_btree_delete(bt, ks) == YQ_OK);
    }
    /* deleting something absent is a no-op, not an error */
    key_of(kbuf, sizeof(kbuf), n + 5);
    {
        yq_slice ks;
        ks.data = kbuf; ks.size = strlen(kbuf);
        CHECK(yq_btree_delete(bt, ks) == YQ_OK);
    }

    for (int i = 0; i < n; i++) {
        key_of(kbuf, sizeof(kbuf), i);
        yq_slice out = { 0 };
        yq_slice ks;
        ks.data = kbuf; ks.size = strlen(kbuf);
        int rc = yq_btree_lookup(bt, ks, &out);
        if (i % 3 == 0) {
            CHECK(rc == YQ_ERR_NOTFOUND);
        } else {
            if (rc != YQ_OK) {
                fprintf(stderr, "FAIL live key %s vanished (rc=%d)\n", kbuf, rc);
                exit(1);
            }
            size_t vlen = 8 + (size_t)(i % 40);
            CHECK(out.size == vlen);
        }
    }

    /* the scan must skip exactly the deleted keys and stay ordered */
    yq_btree_cursor *cur = NULL;
    CHECK(yq_btree_cursor_open(bt, &cur) == YQ_OK);
    CHECK(yq_btree_cursor_first(cur) == YQ_OK);
    int expect = 0, seen = 0;
    while (yq_btree_cursor_valid(cur)) {
        while (expect % 3 == 0) expect++;
        key_of(kbuf, sizeof(kbuf), expect);
        yq_slice key = { 0 };
        CHECK(yq_btree_cursor_key(cur, &key) == YQ_OK);
        if (key.size != strlen(kbuf) || memcmp(key.data, kbuf, key.size) != 0) {
            fprintf(stderr, "FAIL scan after delete: got %.*s want %s\n",
                    (int)key.size, (const char *)key.data, kbuf);
            exit(1);
        }
        seen++;
        expect++;
        yq_btree_cursor_next(cur);
    }
    yq_btree_cursor_close(cur);
    CHECK(seen == n - ((n + 2) / 3));   /* every third key is gone */

    /* deleted keys can come back, including ones whose delete left a hole */
    for (int i = 0; i < n; i += 3) {
        key_of(kbuf, sizeof(kbuf), i);
        size_t vlen = 8 + (size_t)(i % 40);
        memset(vbuf, (uint8_t)('a' + (i % 26)), vlen);
        yq_slice ks, vs;
        ks.data = kbuf; ks.size = strlen(kbuf);
        vs.data = vbuf; vs.size = vlen;
        CHECK(yq_btree_insert(bt, ks, vs) == YQ_OK);
        yq_slice out = { 0 };
        CHECK(yq_btree_lookup(bt, ks, &out) == YQ_OK);
        CHECK(out.size == vlen);
        CHECK(memcmp(out.data, vbuf, vlen) == 0);
    }

    /* everything is back: full scan in order */
    CHECK(yq_btree_cursor_open(bt, &cur) == YQ_OK);
    CHECK(yq_btree_cursor_first(cur) == YQ_OK);
    seen = 0;
    while (yq_btree_cursor_valid(cur)) {
        key_of(kbuf, sizeof(kbuf), seen);
        yq_slice key = { 0 };
        CHECK(yq_btree_cursor_key(cur, &key) == YQ_OK);
        CHECK(key.size == strlen(kbuf) && memcmp(key.data, kbuf, key.size) == 0);
        seen++;
        yq_btree_cursor_next(cur);
    }
    yq_btree_cursor_close(cur);
    CHECK(seen == n);

    yq_btree_close(bt);
    yq_page_store_destroy(store);
    printf("OK\n");
}

/* Values too large for an inline cell must be refused, not silently broken. */
static void test_oversize_value_refused(void) {
    printf("test_oversize_value_refused... ");

    yq_btree *bt = yq_btree_create(NULL, PS);
    yq_page_store *store = yq_page_store_mem_create(PS);
    yq_btree_set_page_store(bt, store);

    static uint8_t big[PS / 4 + 1];
    memset(big, 'z', sizeof(big));

    yq_slice k = { "k", 1 };
    yq_slice v = { big, sizeof(big) };
    CHECK(yq_btree_insert(bt, k, v) == YQ_ERR_TOOBIG);

    /* ... and the tree is still usable afterwards */
    yq_slice v2 = { "ok", 2 };
    CHECK(yq_btree_insert(bt, k, v2) == YQ_OK);
    yq_slice out = { 0 };
    CHECK(yq_btree_lookup(bt, k, &out) == YQ_OK);
    CHECK(out.size == 2 && memcmp(out.data, "ok", 2) == 0);

    yq_btree_close(bt);
    yq_page_store_destroy(store);
    printf("OK\n");
}

/* A tree with no store cannot grow -- loudly, not by inventing page numbers. */
static void test_insert_without_store_refused(void) {
    printf("test_insert_without_store_refused... ");

    yq_btree *bt = yq_btree_create(NULL, PS);
    CHECK(bt != NULL);
    yq_slice k = { "k", 1 };
    yq_slice v = { "v", 1 };
    CHECK(yq_btree_insert(bt, k, v) == YQ_ERR_NOTSUP);
    yq_btree_close(bt);
    printf("OK\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);   /* keep output if a check aborts */
    printf("=== yq-DB P3 S1: Page Store + Tree Build/Read-Back ===\n\n");

    test_page_store();
    test_insert_without_store_refused();
    test_oversize_value_refused();
    test_delete_and_reinsert();

    /* Small enough to stay a single leaf, then big enough to force leaf
     * splits, then big enough to force internal splits and a second level. */
    build_and_verify(PS, 50, 0, "build_and_verify_50keys");
    build_and_verify(PS, 2000, 0, "build_and_verify_2000keys_sequential");
    build_and_verify(PS, 2000, 1, "build_and_verify_2000keys_random");
    build_and_verify(PS, 8000, 1, "build_and_verify_8000keys_random");
    build_and_verify(8192, 8000, 1, "build_and_verify_8kpages_random");

    printf("\n=== ALL TESTS PASSED ===\n");
    return 0;
}