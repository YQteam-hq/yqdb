#ifndef YQ_MEMTABLE_H
#define YQ_MEMTABLE_H
#include <stddef.h>
#include <stdint.h>
#include "yq.h"

typedef struct yq_memtable yq_memtable;
typedef struct yq_memtable_iter yq_memtable_iter;

yq_memtable *yq_memtable_create(size_t max_bytes);
void yq_memtable_destroy(yq_memtable *mt);
int yq_memtable_put(yq_memtable *mt, yq_slice key, yq_slice val);
int yq_memtable_del(yq_memtable *mt, yq_slice key);
int yq_memtable_get(yq_memtable *mt, yq_slice key, yq_slice *out);
int yq_memtable_size(yq_memtable *mt);
int yq_memtable_full(yq_memtable *mt);
size_t yq_memtable_bytes(yq_memtable *mt);
int yq_memtable_iter_open(yq_memtable *mt, yq_memtable_iter **out);
void yq_memtable_iter_close(yq_memtable_iter *it);
int yq_memtable_iter_first(yq_memtable_iter *it);
int yq_memtable_iter_next(yq_memtable_iter *it);
int yq_memtable_iter_last(yq_memtable_iter *it);
int yq_memtable_iter_prev(yq_memtable_iter *it);
/*
 * Position the iterator at the first entry whose key is >= `target`,
 * skipping tombstones the way iter_first()/iter_next() do.
 *
 * This is a binary search. Walking from iter_first() and comparing each key
 * in turn is linear in the number of entries, which makes every yq_cur_seek()
 * and yq_cur_seek_exact() call O(n) in the size of the memtable.
 *
 * Returns YQ_OK with the iterator positioned, or YQ_ERR_NOTFOUND when every
 * key is smaller than `target` (in which case the iterator is left past the
 * end and iter_valid() reports 0).
 */
int yq_memtable_iter_seek(yq_memtable_iter *it, yq_slice target);
int yq_memtable_iter_key(yq_memtable_iter *it, yq_slice *out);
int yq_memtable_iter_val(yq_memtable_iter *it, yq_slice *out);
int yq_memtable_iter_valid(yq_memtable_iter *it);
void yq_memtable_reset(yq_memtable *mt);

#endif
