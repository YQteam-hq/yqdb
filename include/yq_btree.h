#ifndef YQ_BTREE_H
#define YQ_BTREE_H

#include <stddef.h>
#include <stdint.h>
#include "yq.h"
#include "yq_memblk.h"
#include "yq_page.h"

typedef struct yq_btree yq_btree;
typedef struct yq_btree_cursor yq_btree_cursor;

yq_btree *yq_btree_create(yq_memblk *arena, uint32_t page_size);
void yq_btree_destroy(yq_btree *bt);
int yq_btree_insert(yq_btree *bt, yq_slice key, yq_slice val);
int yq_btree_lookup(yq_btree *bt, yq_slice key, yq_slice *out);
int yq_btree_delete(yq_btree *bt, yq_slice key);
int yq_btree_open(yq_btree **out, void *mmap_base, uint64_t file_size, uint32_t page_size);
/*
 * Release a handle returned by yq_btree_create() or yq_btree_open().
 *
 * The arena is owned by the caller, not by the handle: yq_open() passes
 * either an mmap()ed region or a heap block, and it is the caller that knows
 * which one it is, so yq_btree_close() never touches bt->arena.
 */
void yq_btree_close(yq_btree *bt);
int yq_btree_get_root(yq_btree *bt, uint64_t *root_page);
int yq_btree_set_root(yq_btree *bt, uint64_t root_page);
int yq_btree_set_npages(yq_btree *bt, uint64_t npages);
int yq_btree_cursor_open(yq_btree *bt, yq_btree_cursor **c);
void yq_btree_cursor_close(yq_btree_cursor *c);
int yq_btree_cursor_first(yq_btree_cursor *c);
int yq_btree_cursor_last(yq_btree_cursor *c);
int yq_btree_cursor_next(yq_btree_cursor *c);
int yq_btree_cursor_prev(yq_btree_cursor *c);
int yq_btree_cursor_key(yq_btree_cursor *c, yq_slice *out);
int yq_btree_cursor_val(yq_btree_cursor *c, yq_slice *out);
int yq_btree_cursor_valid(yq_btree_cursor *c);
int yq_btree_cursor_seek(yq_btree_cursor *c, yq_slice key);
int yq_btree_cursor_seek_le(yq_btree_cursor *c, yq_slice key);

uint64_t yq_btree_npages(yq_btree *bt);
/*
 * Bind a page store (P3). Page numbers handed to the tree come from it, and
 * every page access goes through it; without a store the tree falls back to
 * the legacy arena/mmap paths, which cannot allocate pages out of the
 * database file and therefore cannot spill.
 */
void yq_btree_set_page_store(yq_btree *bt, yq_page_store *store);
yq_page_store *yq_btree_page_store(yq_btree *bt);
/* 1 when the tree has a root page at all (root_page 0 means "empty"). */
int yq_btree_has_root(yq_btree *bt);
void yq_btree_set_page_provider(yq_btree *bt, void *ctx,
    void *(*alloc)(void *ctx, int is_leaf),
    void (*free)(void *ctx, uint64_t page_no));
void yq_btree_set_file_provider(yq_btree *bt, void *ctx,
    void *(*alloc)(void *ctx, int is_leaf, uint64_t *out_page_no),
    void (*sync)(void *ctx));
int yq_btree_flush_page(yq_btree *bt, uint64_t page_no, const void *page_data);
void yq_btree_sync(yq_btree *bt);

#endif
