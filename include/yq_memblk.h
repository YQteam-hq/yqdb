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
#endif
