#ifndef YQ_MEMBLK_H
#define YQ_MEMBLK_H
#include <stddef.h>
#include <stdint.h>
typedef struct yq_memblk yq_memblk;
yq_memblk *yq_memblk_create(size_t size);
void yq_memblk_destroy(yq_memblk *b);
void *yq_memblk_alloc(yq_memblk *b, size_t size);
void yq_memblk_reset(yq_memblk *b);
void *yq_memblk_base(yq_memblk *b);
size_t yq_memblk_used(yq_memblk *b);
size_t yq_memblk_size(yq_memblk *b);

/*
 * Grow the block to at least new_size and return the new block (the old
 * handle must not be used afterwards). The `used` bytes are copied to the
 * same offsets in the new block, so every offset handed out by
 * yq_memblk_alloc() stays valid across the grow -- which is what makes the
 * memtable's offset-based entries survive an arena resize.
 *
 * Returns NULL on allocation failure (the original block is left untouched
 * and remains usable). A block large enough already is returned as-is.
 *
 * Note: the `next` link is not involved. yq_memblk_alloc() never chains
 * blocks today; chaining would break the offset model (offsets are relative
 * to the first block's base), so growth relocates instead.
 */
yq_memblk *yq_memblk_grow(yq_memblk *b, size_t new_size);

#endif
