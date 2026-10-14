/*
 * yq_page.h -- page store: the allocator that gives the B+Tree page numbers.
 *
 * P3 stage 1. The B+Tree could not grow because alloc_page() handed out raw
 * buffers with no page number behind them (page numbers were invented as
 * bt->npages++), so nothing ever allocated pages out of the database file.
 * This module owns that job: page numbers, a free list, and the address of
 * a page's bytes.
 *
 * Page numbering follows FORMAT.md: pages 0 and 1 are the meta pages, data
 * pages start at 2, and 0 doubles as the "no page" sentinel -- which is why
 * the free list can be a plain singly linked list threaded through the freed
 * pages themselves (the first 8 bytes, the same field the format reuses as
 * "next free page").
 *
 * The store is the seam between an in-memory tree and a persisted one: stage 1
 * ships the memory implementation, stage 2 adds a file-backed one behind the
 * same three operations (alloc / get / free).
 */
#ifndef YQ_PAGE_H
#define YQ_PAGE_H

#include <stddef.h>
#include <stdint.h>

typedef struct yq_page_store yq_page_store;

/*
 * In-memory store: one page_size buffer per page number. Page 0 and 1 are
 * reserved for the meta pages and never handed out.
 */
yq_page_store *yq_page_store_mem_create(uint32_t page_size);
void yq_page_store_destroy(yq_page_store *st);

/*
 * Take a page: from the free list if one is available, otherwise by
 * extending the store. The returned page is zeroed. YQ_ERR_MAP_FULL-style
 * exhaustion surfaces as YQ_ERR_NOMEM (the memory store can only fail on
 * allocation).
 */
int yq_page_store_alloc(yq_page_store *st, uint64_t *out_page_no);

/*
 * Address of an allocated page, NULL when the number was never handed out or
 * has been freed. Callers treat NULL as a corrupt/out-of-range reference.
 */
void *yq_page_store_get(yq_page_store *st, uint64_t page_no);

/*
 * Return a page to the free list. Its contents are irrelevant afterwards; the
 * next-free pointer is written over the first 8 bytes. Returns YQ_ERR_INVAL
 * for page 0/1 (reserved) or unknown pages.
 */
int yq_page_store_free(yq_page_store *st, uint64_t page_no);

/* High-water mark: file length / page_size, i.e. pages 0..npages-1 exist. */
uint64_t yq_page_store_npages(const yq_page_store *st);
uint64_t yq_page_store_free_head(const yq_page_store *st);

/*
 * Adopt a page count discovered elsewhere (a reopened database file knows its
 * own length). The array grows to cover it without allocating buffers: those
 * pages exist in the file but have not been read yet. Never shrinks, since a
 * live free list would be lost.
 */
int yq_page_store_set_npages(yq_page_store *st, uint64_t npages);

/* Pages currently allocated (npages - free list length); for tests/stats. */
uint64_t yq_page_store_live(const yq_page_store *st);
uint32_t yq_page_store_page_size(const yq_page_store *st);

#endif