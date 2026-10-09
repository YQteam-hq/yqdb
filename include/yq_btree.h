#ifndef YQ_BTREE_H
#define YQ_BTREE_H

#include <stddef.h>
#include <stdint.h>
#include "yq.h"
#include "yq_memblk.h"

typedef struct yq_btree yq_btree;
typedef struct yq_btree_cursor yq_btree_cursor;

yq_btree *yq_btree_create(yq_memblk *arena, uint32_t page_size);
void yq_btree_destroy(yq_btree *bt);
int yq_btree_insert(yq_btree *bt, yq_slice key, yq_slice val);
int yq_btree_lookup(yq_btree *bt, yq_slice key, yq_slice *out);
int yq_btree_delete(yq_btree *bt, yq_slice key);
int yq_btree_open(yq_btree **out, void *mmap_base, uint64_t file_size, uint32_t page_size);
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
void yq_btree_set_page_provider(yq_btree *bt, void *ctx,
    void *(*alloc)(void *ctx, int is_leaf),
    void (*free)(void *ctx, uint64_t page_no));
void yq_btree_set_file_provider(yq_btree *bt, void *ctx,
    void *(*alloc)(void *ctx, int is_leaf, uint64_t *out_page_no),
    void (*sync)(void *ctx));
int yq_btree_flush_page(yq_btree *bt, uint64_t page_no, const void *page_data);
void yq_btree_sync(yq_btree *bt);

#endif
