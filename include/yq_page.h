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
#include "yq_vfs.h"

typedef struct yq_page_store yq_page_store;

/*
 * File-backed store (P3 stage 2): the same three operations, with page bytes
 * that live in a file.
 *
 * Pages stay in buffers exactly as the memory store keeps them -- the tree's
 * insert path holds pointers to a leaf, its parent and a fresh sibling at the
 * same time, so a page must not move under it, which rules out growing an
 * mmap() underneath a live tree. Instead, a page is read on first touch
 * (pread at page_no * page_size) and written back in batches by
 * yq_page_store_sync(), which also rewrites the free-list links.
 *
 * Durability is the caller's to ask for: nothing is written until sync(), and
 * nothing is ordered before yq_file_sync() by sync() itself.
 *
 * A page that exists in the file but has not been read yet reports NULL from
 * yq_page_store_get() like a freed page; yq_page_store_touch() distinguishes
 * them, which is how a reopened database reads its tree lazily.
 */
yq_page_store *yq_page_store_file_create(yq_file *fh, uint32_t page_size);

/*
 * Open a store over an existing file, adopting its page count and rebuilding
 * the free list by walking the chain FORMAT.md section 4.4 threads through the
 * freed pages' first 8 bytes. npages/free_head come from the meta page, so the
 * caller reads them from there.
 */
yq_page_store *yq_page_store_file_open(yq_file *fh, uint32_t page_size,
                                       uint64_t npages, uint64_t free_head);

/*
 * Write back every dirty page, rewrite the free-list links and sync the file.
 * Returns YQ_OK when there is no file behind the store (nothing to do).
 */
int yq_page_store_sync(yq_page_store *st);

/*
 * Make sure pages[page_no] is populated, reading it from the file when it is
 * there but unread. Returns NULL when the page was never allocated or has been
 * freed. yq_page_store_get() is the same call without the read-back.
 */
void *yq_page_store_touch(yq_page_store *st, uint64_t page_no);

/* The file behind the store, or NULL for the memory store. */
yq_file *yq_page_store_file(yq_page_store *st);

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

/*
 * Return every page in [2, npages) to the free list without shrinking the
 * file. Used to rebuild the tree from a fresh sorted run: the pages exist but
 * their contents are about to be replaced wholesale, so the numbers stay
 * allocated to the file while the buffers go back to the allocator.
 */
int yq_page_store_reset(yq_page_store *st);

#endif