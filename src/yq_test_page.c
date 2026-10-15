/*
 * Tests for the page store (P3 stage 1 memory store, stage 2 file-backed).
 *
 * The store is the whole reason the tree can grow: it is what turns "a page
 * buffer" into "a page number that means something", so the properties tested
 * here are the ones the tree's write path depends on and the ones a reopened
 * database depends on.
 *
 *   - page numbers are dense and never reused while allocated
 *   - a freed page number stops resolving (get -> NULL), so a stale reference
 *     cannot read the free-list link as page contents
 *   - the file-backed store writes back only at sync(), and a reopen reads
 *     pages lazily -- get() is NULL until the page is touched, which is what
 *     tells "freed" apart from "in the file, not read yet"
 *   - the free list survives close/reopen through the chain the format
 *     threads through the freed pages
 *   - a free list that disagrees with the file is refused, not guessed at
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "yq_page.h"
#include "yq_vfs.h"

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

#define TEST_FILE "page_store.tmp"

static void rm(void) { remove(TEST_FILE); }

/* Fill a page with a recognizable pattern derived from its number. */
static void stamp(uint8_t *p, uint32_t ps, uint64_t no) {
    memset(p, 0, ps);
    p[0] = (uint8_t)no;
    p[1] = (uint8_t)(no >> 8);
    p[2] = 0xA5;
    memcpy(p + 8, "PAGEDATA", 8);
}

static int matches(const uint8_t *p, uint64_t no) {
    uint64_t back = (uint64_t)p[0] | ((uint64_t)p[1] << 8);
    return back == no && p[2] == 0xA5 && memcmp(p + 8, "PAGEDATA", 8) == 0;
}

/* ── memory store ────────────────────────────────────────────────────── */

static void test_mem_store_basics(void) {
    printf("test_mem_store_basics... ");

    yq_page_store *st = yq_page_store_mem_create(4096);
    CHECK(st != NULL);

    uint64_t a, b, c;
    CHECK(yq_page_store_alloc(st, &a) == YQ_OK);
    CHECK(yq_page_store_alloc(st, &b) == YQ_OK);
    CHECK(yq_page_store_alloc(st, &c) == YQ_OK);

    /* Dense, and pages 0/1 are the meta pages so they are never handed out. */
    CHECK(a == 2 && b == 3 && c == 4);
    CHECK(yq_page_store_npages(st) == 5);
    CHECK(yq_page_store_live(st) == 3);
    CHECK(yq_page_store_free_head(st) == 0);

    uint8_t *pa = (uint8_t *)yq_page_store_get(st, a);
    CHECK(pa != NULL);
    stamp(pa, 4096, a);

    /* Freing detaches the buffer: a stale page number must stop resolving. */
    CHECK(yq_page_store_free(st, b) == YQ_OK);
    CHECK(yq_page_store_get(st, b) == NULL);
    CHECK(yq_page_store_live(st) == 2);
    CHECK(yq_page_store_free_head(st) == b);
    CHECK(yq_page_store_get(st, a) != NULL);     /* neighbours untouched */

    /* Freeing twice, or the meta pages, is an error. */
    CHECK(yq_page_store_free(st, b) == YQ_ERR_INVAL);
    CHECK(yq_page_store_free(st, 0) == YQ_ERR_INVAL);
    CHECK(yq_page_store_free(st, 1) == YQ_ERR_INVAL);
    CHECK(yq_page_store_free(st, 4096) == YQ_ERR_INVAL);

    /* The number comes back from the free list, zeroed. */
    uint64_t again;
    CHECK(yq_page_store_alloc(st, &again) == YQ_OK);
    CHECK(again == b);
    uint8_t *pb = (uint8_t *)yq_page_store_get(st, again);
    CHECK(pb != NULL);
    CHECK(pb[0] == 0 && pb[2] == 0 && memcmp(pb + 8, "PAGEDATA", 8) != 0);

    /* No file behind it: sync has nothing to do and must still succeed. */
    CHECK(yq_page_store_sync(st) == YQ_OK);

    /* adopt_npages never shrinks. */
    CHECK(yq_page_store_set_npages(st, 4) == YQ_ERR_INVAL);
    CHECK(yq_page_store_set_npages(st, 64) == YQ_OK);
    CHECK(yq_page_store_npages(st) == 64);

    yq_page_store_destroy(st);
    printf("OK\n");
}

/* ── file-backed store ────────────────────────────────────────────────── */

static void test_file_store_roundtrip(void) {
    printf("test_file_store_roundtrip... ");

    rm();
    yq_file *fh = yq_file_open(TEST_FILE, 1, 1);
    CHECK(fh != NULL);
    /* The two meta pages exist, as they do in a real database file. */
    CHECK(yq_file_truncate(fh, 2 * 4096) == YQ_OK);

    yq_page_store *st = yq_page_store_file_create(fh, 4096);
    CHECK(st != NULL);
    CHECK(yq_page_store_file(st) == fh);

    uint64_t p[5];
    for (int i = 0; i < 5; i++) {
        CHECK(yq_page_store_alloc(st, &p[i]) == YQ_OK);
        stamp((uint8_t *)yq_page_store_get(st, p[i]), 4096, p[i]);
    }
    CHECK(yq_page_store_npages(st) == 7);

    /*
     * Before sync the file is untouched: durability is asked for, not assumed.
     * This is the property that keeps a file write from being half-finished
     * while the tree is mid-insert.
     */
    CHECK(yq_file_size(fh) == 2 * 4096);
    CHECK(yq_page_store_sync(st) == YQ_OK);
    CHECK(yq_file_size(fh) == 7 * 4096);

    yq_page_store_destroy(st);       /* the file handle is owned by the caller */
    yq_file_close(fh);

    /* Reopen: adopt the page count and read pages lazily. */
    yq_file *fh2 = yq_file_open(TEST_FILE, 0, 0);
    CHECK(fh2 != NULL);
    yq_page_store *st2 = yq_page_store_file_open(fh2, 4096, 7, 0);
    CHECK(st2 != NULL);
    CHECK(yq_page_store_npages(st2) == 7);
    CHECK(yq_page_store_live(st2) == 5);
    CHECK(yq_page_store_free_head(st2) == 0);

    /* Not read yet -- and that is distinguishable from freed. */
    CHECK(yq_page_store_get(st2, p[0]) == NULL);
    for (int i = 0; i < 5; i++) {
        uint8_t *q = (uint8_t *)yq_page_store_touch(st2, p[i]);
        CHECK(q != NULL);
        CHECK(matches(q, p[i]));
    }

    yq_page_store_destroy(st2);
    yq_file_close(fh2);
    rm();
    printf("OK\n");
}

static void test_file_store_free_list_survives(void) {
    printf("test_file_store_free_list_survives... ");

    rm();
    yq_file *fh = yq_file_open(TEST_FILE, 1, 1);
    CHECK(fh != NULL);
    CHECK(yq_file_truncate(fh, 2 * 4096) == YQ_OK);

    yq_page_store *st = yq_page_store_file_create(fh, 4096);
    CHECK(st != NULL);

    uint64_t p[6];
    for (int i = 0; i < 6; i++) {
        CHECK(yq_page_store_alloc(st, &p[i]) == YQ_OK);
        stamp((uint8_t *)yq_page_store_get(st, p[i]), 4096, p[i]);
    }

    /* Free three of them; the chain order is the reverse of the frees. */
    CHECK(yq_page_store_free(st, p[1]) == YQ_OK);
    CHECK(yq_page_store_free(st, p[3]) == YQ_OK);
    CHECK(yq_page_store_free(st, p[5]) == YQ_OK);
    CHECK(yq_page_store_free_head(st) == p[5]);
    CHECK(yq_page_store_live(st) == 3);

    CHECK(yq_page_store_sync(st) == YQ_OK);

    yq_page_store_destroy(st);
    yq_file_close(fh);

    /* Reopen from the meta page's view of the world: 8 pages, head = p[5]. */
    yq_file *fh2 = yq_file_open(TEST_FILE, 0, 0);
    CHECK(fh2 != NULL);
    yq_page_store *st2 = yq_page_store_file_open(fh2, 4096, 8, p[5]);
    CHECK(st2 != NULL);
    CHECK(yq_page_store_free_head(st2) == p[5]);
    CHECK(yq_page_store_live(st2) == 3);

    /* The freed pages resolve to nothing; the live ones come back intact. */
    for (int i = 0; i < 6; i++) {
        uint8_t *q = (uint8_t *)yq_page_store_touch(st2, p[i]);
        if (i == 1 || i == 3 || i == 5) {
            CHECK(q == NULL);
            CHECK(yq_page_store_get(st2, p[i]) == NULL);
        } else {
            CHECK(q != NULL);
            CHECK(matches(q, p[i]));
        }
    }

    /* Allocation reuses the freed numbers, LIFO. */
    uint64_t r1, r2, r3;
    CHECK(yq_page_store_alloc(st2, &r1) == YQ_OK);
    CHECK(yq_page_store_alloc(st2, &r2) == YQ_OK);
    CHECK(yq_page_store_alloc(st2, &r3) == YQ_OK);
    CHECK(r1 == p[5] && r2 == p[3] && r3 == p[1]);
    CHECK(yq_page_store_live(st2) == 6);

    /* A page freed while it lives only in the file (never touched) is taken
     * too: get() would have said NULL, which must not mean "already free". */
    yq_page_store *st3 = yq_page_store_file_open(fh2, 4096, 8, 0);
    CHECK(st3 != NULL);
    uint64_t keep;
    CHECK(yq_page_store_alloc(st3, &keep) == YQ_OK);
    CHECK(yq_page_store_free(st3, keep) == YQ_OK);
    CHECK(yq_page_store_free(st3, keep) == YQ_ERR_INVAL);

    yq_page_store_destroy(st3);
    yq_page_store_destroy(st2);
    yq_file_close(fh2);
    rm();
    printf("OK\n");
}

static void test_file_store_rejects_bad_free_list(void) {
    printf("test_file_store_rejects_bad_free_list... ");

    rm();
    yq_file *fh = yq_file_open(TEST_FILE, 1, 1);
    CHECK(fh != NULL);
    CHECK(yq_file_truncate(fh, 8 * 4096) == YQ_OK);

    yq_page_store *st = yq_page_store_file_create(fh, 4096);
    CHECK(st != NULL);
    uint64_t p[3];
    for (int i = 0; i < 3; i++) {
        CHECK(yq_page_store_alloc(st, &p[i]) == YQ_OK);
        stamp((uint8_t *)yq_page_store_get(st, p[i]), 4096, p[i]);
    }
    CHECK(yq_page_store_free(st, p[0]) == YQ_OK);
    CHECK(yq_page_store_sync(st) == YQ_OK);
    yq_page_store_destroy(st);
    yq_file_close(fh);

    /* A head past the end of the file: refuse rather than hand out a page
     * number that does not exist. */
    yq_file *fh2 = yq_file_open(TEST_FILE, 0, 0);
    CHECK(fh2 != NULL);
    CHECK(yq_page_store_file_open(fh2, 4096, 8, 9999) == NULL);
    /* A head that points at a meta page is just as wrong. */
    CHECK(yq_page_store_file_open(fh2, 4096, 8, 1) == NULL);
    /* And the honest view of the file still opens. */
    yq_page_store *good = yq_page_store_file_open(fh2, 4096, 8, p[0]);
    CHECK(good != NULL);
    CHECK(yq_page_store_free_head(good) == p[0]);
    yq_page_store_destroy(good);

    yq_file_close(fh2);
    rm();
    printf("OK\n");
}

int main(void) {
    printf("=== yq-DB Page Store Tests (P3-S1/S2) ===\n\n");

    test_mem_store_basics();
    test_file_store_roundtrip();
    test_file_store_free_list_survives();
    test_file_store_rejects_bad_free_list();

    printf("\n=== ALL TESTS PASSED ===\n");
    return 0;
}
