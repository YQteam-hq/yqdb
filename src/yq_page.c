#include "yq_page.h"
#include "yq.h"

#include <stdlib.h>
#include <string.h>

/*
 * In-memory page store (P3 stage 1).
 *
 * pages[i] is the buffer for page number i, NULL when that number was never
 * allocated or has been freed. The array grows with the high-water mark, so
 * page numbers stay dense and directly indexable -- which is what lets the
 * B+Tree treat a page number as an offset into "the file" without knowing
 * whether that file is on disk yet.
 */

#define YQ_FIRST_DATA_PAGE 2u

struct yq_page_store {
    uint32_t page_size;
    uint64_t npages;         /* high-water mark; pages [0, npages) exist */
    uint64_t free_head;      /* 0 = empty */
    uint64_t live;           /* allocated pages, for tests/stats */
    uint8_t **pages;         /* pages[i] -> buffer, NULL when not allocated */
    /*
     * Next link for the free list, kept in a parallel array rather than
     * inside the page itself: the buffer address has to survive free() for
     * the list to walk, while get() must report a freed page as gone. (The
     * on-disk format threads this through the page; in memory the two
     * requirements simply cannot share storage.)
     */
    uint64_t *free_next;
    size_t cap;              /* capacity of the pages[]/free_next[] arrays */
};

yq_page_store *yq_page_store_mem_create(uint32_t page_size) {
    if (page_size == 0) page_size = 4096;
    yq_page_store *st = (yq_page_store *)calloc(1, sizeof(yq_page_store));
    if (!st) return NULL;

    st->page_size = page_size;
    st->npages = YQ_FIRST_DATA_PAGE;   /* 0 and 1 are the meta pages */
    st->free_head = 0;
    st->live = 0;
    st->cap = 0;
    st->pages = NULL;
    return st;
}

void yq_page_store_destroy(yq_page_store *st) {
    if (!st) return;
    for (size_t i = 0; i < st->cap; i++) free(st->pages[i]);
    free(st->pages);
    free(st->free_next);
    free(st);
}

static int store_grow(yq_page_store *st, uint64_t need) {
    if (need <= st->cap) return YQ_OK;
    size_t ncap = st->cap ? st->cap : 64;
    while (ncap < need) ncap *= 2;
    uint8_t **np = (uint8_t **)realloc(st->pages, ncap * sizeof(uint8_t *));
    if (!np) return YQ_ERR_NOMEM;
    memset(np + st->cap, 0, (ncap - st->cap) * sizeof(uint8_t *));
    st->pages = np;

    uint64_t *nn = (uint64_t *)realloc(st->free_next, ncap * sizeof(uint64_t));
    if (!nn) return YQ_ERR_NOMEM;
    memset(nn + st->cap, 0, (ncap - st->cap) * sizeof(uint64_t));
    st->free_next = nn;

    st->cap = ncap;
    return YQ_OK;
}

int yq_page_store_alloc(yq_page_store *st, uint64_t *out_page_no) {
    if (!st || !out_page_no) return YQ_ERR_INVAL;

    uint64_t page_no;
    uint8_t *buf;

    if (st->free_head != 0) {
        /* Reuse a page number; free() already released its buffer. */
        page_no = st->free_head;
        st->free_head = st->free_next[page_no];
        buf = st->pages[page_no];
        if (!buf) {
            buf = (uint8_t *)malloc(st->page_size);
            if (!buf) return YQ_ERR_NOMEM;
            st->pages[page_no] = buf;
        }
    } else {
        page_no = st->npages;
        int rc = store_grow(st, page_no + 1);
        if (rc != YQ_OK) return rc;
        buf = (uint8_t *)malloc(st->page_size);
        if (!buf) return YQ_ERR_NOMEM;
        st->pages[page_no] = buf;
        st->npages = page_no + 1;
    }

    memset(buf, 0, st->page_size);
    st->live++;
    *out_page_no = page_no;
    return YQ_OK;
}

void *yq_page_store_get(yq_page_store *st, uint64_t page_no) {
    if (!st || page_no < YQ_FIRST_DATA_PAGE) return NULL;
    if (page_no >= st->npages || page_no >= st->cap) return NULL;
    return st->pages[page_no];   /* NULL when allocated-then-freed */
}

int yq_page_store_free(yq_page_store *st, uint64_t page_no) {
    if (!st) return YQ_ERR_INVAL;
    if (page_no < YQ_FIRST_DATA_PAGE) return YQ_ERR_INVAL;   /* meta pages */
    if (page_no >= st->npages || page_no >= st->cap) return YQ_ERR_INVAL;
    if (!st->pages[page_no]) return YQ_ERR_INVAL;            /* already free */

    st->free_next[page_no] = st->free_head;
    st->free_head = page_no;
    st->live--;
    /*
     * Detach the buffer so a stale page number stops resolving: handing back
     * the freed page's bytes would let a caller read the free-list link as if
     * it were page contents -- the failure mode this store exists to prevent
     * (the old tree invented page numbers and read unrelated memory).
     */
    free(st->pages[page_no]);
    st->pages[page_no] = NULL;
    return YQ_OK;
}

uint64_t yq_page_store_npages(const yq_page_store *st) {
    return st ? st->npages : 0;
}

int yq_page_store_set_npages(yq_page_store *st, uint64_t npages) {
    if (!st) return YQ_ERR_INVAL;
    if (npages < st->npages) return YQ_ERR_INVAL;   /* would orphan the free list */
    if (npages < YQ_FIRST_DATA_PAGE) npages = YQ_FIRST_DATA_PAGE;
    int rc = store_grow(st, npages);
    if (rc != YQ_OK) return rc;
    st->npages = npages;
    return YQ_OK;
}

uint64_t yq_page_store_free_head(const yq_page_store *st) {
    return st ? st->free_head : 0;
}

uint64_t yq_page_store_live(const yq_page_store *st) {
    return st ? st->live : 0;
}

uint32_t yq_page_store_page_size(const yq_page_store *st) {
    return st ? st->page_size : 0;
}