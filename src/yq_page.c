#include "yq_page.h"
#include "yq.h"
#include "yq_vfs.h"

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
 *
 * Stage 2 keeps that and adds a file behind it: see yq_page.h.
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
    /*
     * freed[i] != 0 when page i is on the free list. A NULL pages[i] is
     * ambiguous without it -- "freed" and "exists in the file, not read yet"
     * both look like a missing buffer -- and the difference is what tells
     * touch() whether a pread() has anything to find. The memory store keeps
     * it too: it costs one byte per page and removes a branch.
     */
    uint8_t *freed;
    /* dirty[i] != 0 when pages[i] must be written back (file store only) */
    uint8_t *dirty;
    size_t cap;              /* capacity of the pages[]/free_next[] arrays */
    yq_file *fh;             /* NULL for the memory store */
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
    free(st->freed);
    free(st->dirty);
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

    uint8_t *nf = (uint8_t *)realloc(st->freed, ncap);
    if (!nf) return YQ_ERR_NOMEM;
    memset(nf + st->cap, 0, (ncap - st->cap));
    st->freed = nf;

    uint8_t *nd = (uint8_t *)realloc(st->dirty, ncap);
    if (!nd) return YQ_ERR_NOMEM;
    memset(nd + st->cap, 0, (ncap - st->cap));
    st->dirty = nd;

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
        st->freed[page_no] = 0;
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
    st->dirty[page_no] = 1;   /* zeroed page: worth writing if a file is here */
    st->live++;
    *out_page_no = page_no;
    return YQ_OK;
}

void *yq_page_store_get(yq_page_store *st, uint64_t page_no) {
    if (!st || page_no < YQ_FIRST_DATA_PAGE) return NULL;
    if (page_no >= st->npages || page_no >= st->cap) return NULL;
    return st->pages[page_no];   /* NULL when allocated-then-freed */
}

void *yq_page_store_touch(yq_page_store *st, uint64_t page_no) {
    if (!st || page_no < YQ_FIRST_DATA_PAGE) return NULL;
    if (page_no >= st->npages || page_no >= st->cap) return NULL;
    if (st->pages[page_no]) return st->pages[page_no];
    if (st->freed[page_no] || !st->fh) return NULL;

    /* Exists in the file, not read yet: pull it in on first touch. */
    uint8_t *buf = (uint8_t *)malloc(st->page_size);
    if (!buf) return NULL;
    if (yq_file_pread(st->fh, buf, st->page_size,
                      page_no * (uint64_t)st->page_size) != YQ_OK) {
        free(buf);
        return NULL;
    }
    st->pages[page_no] = buf;
    return buf;
}

yq_file *yq_page_store_file(yq_page_store *st) {
    return st ? st->fh : NULL;
}

int yq_page_store_free(yq_page_store *st, uint64_t page_no) {
    if (!st) return YQ_ERR_INVAL;
    if (page_no < YQ_FIRST_DATA_PAGE) return YQ_ERR_INVAL;   /* meta pages */
    if (page_no >= st->npages || page_no >= st->cap) return YQ_ERR_INVAL;
    if (!st->pages[page_no] && !st->freed[page_no] && st->fh) {
        /* A file-backed page that was never read is free to take as well. */
        st->freed[page_no] = 1;
        st->free_next[page_no] = st->free_head;
        st->free_head = page_no;
        st->live--;
        return YQ_OK;
    }
    if (!st->pages[page_no]) return YQ_ERR_INVAL;            /* already free */

    st->free_next[page_no] = st->free_head;
    st->free_head = page_no;
    st->freed[page_no] = 1;
    st->dirty[page_no] = 0;    /* a freed page's bytes carry the free link only */
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

int yq_page_store_reset(yq_page_store *st) {
    if (!st) return YQ_ERR_INVAL;

    /*
     * Everything in [2, npages) goes onto the free list, newest page first, so
     * the next allocations reuse the highest numbers and the file's live set
     * ends up densely packed at the bottom. The buffers are released: what
     * matters afterwards is the page NUMBERS, and a rebuild writes every one
     * of them before it is read again.
     */
    for (uint64_t i = YQ_FIRST_DATA_PAGE; i < st->npages; i++) {
        if (st->freed[i]) continue;          /* already linked: do not relink */
        st->freed[i] = 1;
        st->dirty[i] = 0;
        free(st->pages[i]);
        st->pages[i] = NULL;
        st->free_next[i] = st->free_head;
        st->free_head = i;
    }
    st->live = 0;
    return YQ_OK;
}

/* ── file-backed store (P3 stage 2) ───────────────────────────────────── */

yq_page_store *yq_page_store_file_create(yq_file *fh, uint32_t page_size) {
    if (!fh) return NULL;

    yq_page_store *st = yq_page_store_mem_create(page_size);
    if (!st) return NULL;
    st->fh = fh;
    return st;
}

yq_page_store *yq_page_store_file_open(yq_file *fh, uint32_t page_size,
                                       uint64_t npages, uint64_t free_head) {
    if (!fh) return NULL;

    yq_page_store *st = yq_page_store_mem_create(page_size);
    if (!st) return NULL;
    st->fh = fh;

    if (yq_page_store_set_npages(st, npages) != YQ_OK) {
        yq_page_store_destroy(st);
        return NULL;
    }

    /*
     * Rebuild the free list from the chain the format threads through the
     * freed pages themselves. Walking it is also the validation: a link that
     * points outside the file, backwards, or at a page already visited means
     * the meta page and the file disagree, and the honest answer is to refuse
     * rather than to hand the tree a page number that aliases a live page.
     */
    uint64_t p = free_head;
    uint64_t walked = 0;
    while (p != 0) {
        if (p < YQ_FIRST_DATA_PAGE || p >= st->npages) goto corrupt;
        if (st->freed[p]) goto corrupt;            /* cycle */
        if (walked++ >= st->npages) goto corrupt;  /* belt and braces */

        uint64_t next = 0;
        if (yq_file_pread(fh, &next, sizeof(next),
                          p * (uint64_t)st->page_size) != YQ_OK) goto corrupt;
        st->freed[p] = 1;
        st->free_next[p] = next;
        p = next;
    }
    st->free_head = free_head;
    /* Every page in [2, npages) exists in the file; the chain says which of
     * them are free. Decrementing per link would underflow, since the count
     * starts at zero here rather than at the page total. */
    st->live = st->npages - YQ_FIRST_DATA_PAGE - walked;
    return st;

corrupt:
    yq_page_store_destroy(st);
    return NULL;
}

int yq_page_store_sync(yq_page_store *st) {
    if (!st || !st->fh) return YQ_OK;   /* memory store: nothing to write */

    int rc = YQ_OK;
    uint64_t ps = st->page_size;

    for (uint64_t i = YQ_FIRST_DATA_PAGE; i < st->npages; i++) {
        if (!st->dirty[i] || !st->pages[i]) continue;
        if (yq_file_pwrite(st->fh, st->pages[i], ps, i * ps) != YQ_OK) rc = YQ_ERR_IO;
        st->dirty[i] = 0;
    }

    /*
     * The free list is threaded through the freed pages' first 8 bytes, so
     * those links have to be rewritten whenever the chain changed. Writing the
     * whole chain is O(free pages); a meta page rewrite would be cheaper but
     * FORMAT.md section 4.4 puts the links in the pages.
     */
    for (uint64_t p = st->free_head; p != 0; p = st->free_next[p]) {
        uint64_t link = st->free_next[p];
        if (yq_file_pwrite(st->fh, &link, sizeof(link), p * ps) != YQ_OK) rc = YQ_ERR_IO;
    }

    /* Make the file length match the high-water mark: a reopen derives the
     * page count from it, and a short file would truncate the tail of the
     * tree without saying so. */
    uint64_t want = st->npages * ps;
    if (yq_file_size(st->fh) < want) {
        if (yq_file_truncate(st->fh, want) != YQ_OK) rc = YQ_ERR_IO;
    }
    if (rc == YQ_OK) rc = yq_file_sync(st->fh);
    return rc;
}
